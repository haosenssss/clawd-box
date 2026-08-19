#!/usr/bin/env bun
/**
 * Clawd Box 守护进程。
 *
 * 只做三件必须在 Mac 上做的事，然后近乎原样转发给板子：
 *   1. 读 ~/.claude/sessions/*.json 注册表（板子读不到 Mac 文件系统）
 *   2. PID 存活校验（板子检查不了 Mac 进程）
 *   3. 接住 statusLine / 钩子推来的数据
 *
 * 会话表、计时、状态机、动画选择、页面路由全在板子上——这边只是元数据提供方。
 *
 * 用法：bun run src/index.ts  [--print]   （--print 不开串口，只打印帧，便于脱离硬件调试）
 */

import { activeOnly, readRegistry, type RegistrySession } from './registry.ts'
import { createDispatcher } from './dispatch.ts'
import { openSerialLink, type SerialLink } from './serial.ts'
import {
  parseContextPct,
  parseStatusLineLimits,
  readPlanUsageHistory,
} from './limits.ts'
import { startIngestServer, type IngestMessage } from './ingest.ts'
import { shortId, type Frame, type SessionStatus } from './protocol.ts'

const REGISTRY_POLL_MS = 500
const LIMITS_FALLBACK_POLL_MS = 60_000
/** 超过这个时长没收到 statusLine 的实时限额，就用缓存兜底。 */
const LIVE_LIMITS_STALE_MS = 5 * 60_000
/**
 * 周期性重发全量快照。
 *
 * 板子重启（重烧固件、掉电、看门狗）时主机这边的 fd 仍然有效，
 * 察觉不到对端已经清空；而会话帧又被去重挡着，结果板子会一直空着
 * 直到恰好有状态变化。定期清一次去重表让系统自愈——
 * 代价只是每 20 秒多发几帧。
 */
const FULL_RESYNC_MS = 20_000

const printOnly = process.argv.includes('--print')

// ---------------------------------------------------------------------------
// 传输层
// ---------------------------------------------------------------------------

let link: SerialLink | null = null
const send = (line: string) => {
  if (printOnly) {
    process.stdout.write(line)
    return
  }
  link?.send(line)
}
const dispatch = createDispatcher(send)

if (!printOnly) {
  link = openSerialLink({
    onStateChange: (state, port) => {
      log(state === 'connected' ? `串口已连接 ${port}` : '串口断开，退避重连中')
      if (state === 'connected') {
        // 板子可能刚重启，清空去重表让下一轮重发全量
        dispatch.reset()
        dispatch.emit({ e: 'hello', ts: Math.floor(Date.now() / 1000) })
      }
    },
    /* 板子的自述状态直接进守护进程日志，不必再另开进程去读串口 */
    onBoardLine: (line) => log(`板 | ${line}`),
  })
}

function log(msg: string): void {
  process.stderr.write(`[clawd-box] ${new Date().toISOString()} ${msg}\n`)
}

// ---------------------------------------------------------------------------
// 会话注册表轮询
// ---------------------------------------------------------------------------

/** 上一轮出现过的短 id，用来检测消失。 */
let previousIds = new Set<string>()

/** 钩子推断出的状态，用于 BG_SESSIONS 关闭时兜底。key 是完整 sessionId。 */
const inferredStatus = new Map<string, SessionStatus>()

function resolveStatus(session: RegistrySession): SessionStatus {
  if (session.status !== null) return session.status
  return inferredStatus.get(session.sessionId) ?? 'idle'
}

async function pollRegistry(): Promise<void> {
  let sessions: readonly RegistrySession[]
  try {
    sessions = activeOnly(await readRegistry())
  } catch (err) {
    log(`注册表读取失败: ${String(err)}`)
    return
  }

  const currentIds = new Set<string>()
  for (const session of sessions) {
    const id = shortId(session.sessionId)
    currentIds.add(id)
    dispatch.session({
      sessionId: session.sessionId,
      name: session.name,
      status: resolveStatus(session),
      updatedAtMs: session.updatedAtMs,
    })
  }

  for (const id of previousIds) {
    if (!currentIds.has(id)) dispatch.sessionGone(id)
  }
  previousIds = currentIds
}

// ---------------------------------------------------------------------------
// 限额：实时优先，缓存兜底
// ---------------------------------------------------------------------------

let liveLimitsAtMs = 0
/** 最近一次发出的限额帧，板子重启后要能立刻补发。 */
let lastLimitsFrame: Frame | null = null

function emitLiveLimits(payload: unknown): void {
  const limits = parseStatusLineLimits(payload)
  if (limits === null) return // 首次 API 响应前 rate_limits 不存在，属正常
  liveLimitsAtMs = limits.atMs
  lastLimitsFrame = {
    e: 'limits',
    h5: limits.fiveHour?.pct ?? null,
    h5r: limits.fiveHour?.resetsAt ?? null,
    w7: limits.sevenDay?.pct ?? null,
    w7r: limits.sevenDay?.resetsAt ?? null,
    src: 'live',
    age: 0,
  }
  dispatch.emit(lastLimitsFrame)
}

async function pollFallbackLimits(): Promise<void> {
  const nowMs = Date.now()

  // 只有在实时数据陈旧时才用 Claude Desktop 的采样兜底
  if (nowMs - liveLimitsAtMs > LIVE_LIMITS_STALE_MS) {
    const sample = await readPlanUsageHistory()
    if (sample !== null) {
      dispatch.emit({
        e: 'limits',
        h5: sample.fiveHourPct,
        h5r: null,
        w7: sample.sevenDayPct,
        w7r: null,
        src: 'cached',
        age: Math.floor((nowMs - sample.atMs) / 1000),
      })
    }
  }

}

// ---------------------------------------------------------------------------
// 钩子事件 → 帧
// ---------------------------------------------------------------------------

function handleIngest(msg: IngestMessage): void {
  if (msg.kind === 'statusline') {
    emitLiveLimits(msg.payload)
    const ctx = parseContextPct(msg.payload)
    if (ctx !== null) {
      dispatch.emit({ e: 'ctx', id: shortId(msg.sessionId), pct: ctx })
    }
    return
  }

  const id = shortId(msg.sessionId)
  switch (msg.event) {
    case 'UserPromptSubmit':
      inferredStatus.set(msg.sessionId, 'busy')
      dispatch.emit({ e: 'prompt', id })
      break

    case 'Stop':
      // 只有主线程的 Stop 才算一轮结束；subagent 的 Stop 走 SubagentStop
      if (msg.agentId !== null) break
      inferredStatus.set(msg.sessionId, 'idle')
      dispatch.emit({ e: 'turn_end', id })
      break

    case 'Notification':
      if (msg.notificationType === 'idle_prompt') {
        inferredStatus.set(msg.sessionId, 'waiting')
        dispatch.emit({ e: 'idle_prompt', id })
      }
      break

    case 'SubagentStart':
      if (msg.agentId === null) break
      dispatch.emit({
        e: 'sub_start',
        id,
        aid: shortId(msg.agentId),
        type: (msg.agentType ?? '?').slice(0, 15),
      })
      break

    case 'SubagentStop':
      if (msg.agentId === null) break
      dispatch.emit({ e: 'sub_stop', id, aid: shortId(msg.agentId) })
      break

    case 'SessionEnd':
      inferredStatus.delete(msg.sessionId)
      dispatch.sessionGone(msg.sessionId)
      break

    default:
      break
  }
}

// ---------------------------------------------------------------------------
// 启动与收尾
// ---------------------------------------------------------------------------

const server = await startIngestServer(handleIngest)
log(printOnly ? '已启动（--print 模式，不开串口）' : '已启动')

dispatch.emit({ e: 'hello', ts: Math.floor(Date.now() / 1000) })
await pollRegistry()
await pollFallbackLimits()

const registryTimer = setInterval(() => void pollRegistry(), REGISTRY_POLL_MS)
const resyncTimer = setInterval(() => {
  dispatch.reset()
  dispatch.emit({ e: 'hello', ts: Math.floor(Date.now() / 1000) })
  // 限额是 60s 定时器发的，板子重启后不补就要空一分钟
  if (lastLimitsFrame !== null) dispatch.emit(lastLimitsFrame)
  else void pollFallbackLimits()
}, FULL_RESYNC_MS)
const limitsTimer = setInterval(() => void pollFallbackLimits(), LIMITS_FALLBACK_POLL_MS)

let shuttingDown = false
async function shutdown(signal: string): Promise<void> {
  if (shuttingDown) return
  shuttingDown = true
  clearInterval(registryTimer)
  clearInterval(limitsTimer)
  clearInterval(resyncTimer)
  const s = dispatch.stats()
  log(
    `收到 ${signal}，退出。已发 ${s.sent} 帧 / 去重 ${s.deduped} / 节流 ${s.throttled} / 超长 ${s.oversize}`,
  )
  link?.close()
  await server.close()
  process.exit(0)
}

process.on('SIGINT', () => void shutdown('SIGINT'))
process.on('SIGTERM', () => void shutdown('SIGTERM'))
