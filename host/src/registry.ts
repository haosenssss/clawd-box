/**
 * 读取 Claude Code 自己维护的会话注册表 `~/.claude/sessions/<pid>.json`。
 *
 * 这是纯本地文件读取，零网络、对 Claude Code 零影响。
 *
 * 三个必须处理的现实（均为本机实测所得）：
 *  1. **陈旧文件不会自动清理**——实测 6 个注册文件里 4 个分别是 2.1/7.3/14.3/19.3 天
 *     前最后活动的"停车"会话（进程还活着但早没在用）。必须按活跃度过滤。
 *  2. **无文件锁**——写入是 truncate-then-write，可能读到半截内容。解析失败就跳过本轮。
 *  3. **`status` 字段挂在 `BG_SESSIONS` 编译期开关后**，可能整个不存在。缺失时返回 null，
 *     由上层用钩子事件推断。
 */

import { readdir, readFile } from 'node:fs/promises'
import { basename, join } from 'node:path'
import { homedir } from 'node:os'
import type { SessionStatus } from './protocol.ts'

export const SESSIONS_DIR = join(homedir(), '.claude', 'sessions')

/**
 * 超过这个时长没更新，移出轮播。
 *
 * 取 12 小时而非更短：开着但暂时没在用的 terminal，`statusUpdatedAt` 不会刷新，
 * 窗口太短会把用户实际开着的多个 Claude Code 筛掉。12 小时覆盖一个工作日，
 * 同时仍能挡住实测存在的 14 天 / 19 天僵尸会话。
 */
export const ACTIVE_WINDOW_MS = 12 * 60 * 60 * 1000

/** 超过这个时长没更新，整条淘汰。 */
export const EVICT_AFTER_MS = 24 * 60 * 60 * 1000

/** 只有 `<数字>.json` 才是注册文件——避免把别的文件当成 PID。 */
const PID_FILE_RE = /^(\d+)\.json$/

export type Liveness = 'active' | 'parked' | 'dead'

export type RegistrySession = {
  readonly pid: number
  readonly sessionId: string
  readonly cwd: string
  readonly name: string
  /** null 表示该构建未启用 BG_SESSIONS，需由钩子事件推断状态。 */
  readonly status: SessionStatus | null
  readonly updatedAtMs: number
  readonly startedAtMs: number
  readonly liveness: Liveness
}

const VALID_STATUS: readonly string[] = ['busy', 'waiting', 'idle']

/** 状态枚举按开放集合解析——未知值降级为 idle，不抛错。 */
function parseStatus(raw: unknown): SessionStatus | null {
  if (typeof raw !== 'string') return null
  return VALID_STATUS.includes(raw) ? (raw as SessionStatus) : 'idle'
}

/** 进程是否还活着。signal 0 只做权限与存在性检查，不实际发信号。 */
export function isPidAlive(pid: number): boolean {
  try {
    process.kill(pid, 0)
    return true
  } catch (err) {
    // EPERM 表示进程存在但不属于我们——仍算活着
    return (err as NodeJS.ErrnoException).code === 'EPERM'
  }
}

function classify(alive: boolean, ageMs: number): Liveness {
  if (!alive) return 'dead'
  if (ageMs > EVICT_AFTER_MS) return 'dead'
  return ageMs > ACTIVE_WINDOW_MS ? 'parked' : 'active'
}

function toSession(raw: unknown, nowMs: number): RegistrySession | null {
  if (typeof raw !== 'object' || raw === null) return null
  const r = raw as Record<string, unknown>

  const pid = typeof r.pid === 'number' ? r.pid : NaN
  const sessionId = typeof r.sessionId === 'string' ? r.sessionId : ''
  if (!Number.isInteger(pid) || sessionId === '') return null

  const cwd = typeof r.cwd === 'string' ? r.cwd : ''
  const startedAtMs = typeof r.startedAt === 'number' ? r.startedAt : 0

  // statusUpdatedAt 最准；退而求其次 updatedAt；再不行用 startedAt
  const updatedAtMs =
    typeof r.statusUpdatedAt === 'number'
      ? r.statusUpdatedAt
      : typeof r.updatedAt === 'number'
        ? r.updatedAt
        : startedAtMs

  // name 可能是用户设的昵称，缺失时用 cwd 的目录名兜底
  const rawName = typeof r.name === 'string' && r.name !== '' ? r.name : basename(cwd)

  return {
    pid,
    sessionId,
    cwd,
    name: rawName === '' ? sessionId.slice(0, 8) : rawName,
    status: parseStatus(r.status),
    updatedAtMs,
    startedAtMs,
    liveness: classify(isPidAlive(pid), nowMs - updatedAtMs),
  }
}

export type RegistrySnapshot = {
  readonly sessions: readonly RegistrySession[]
  /** 本轮因解析失败（多半是 torn read）跳过的文件数。 */
  readonly skipped: number
}

/**
 * 扫一遍注册表目录。**只读**，绝不写入或删除该目录下任何文件
 * ——清理陈旧文件是 Claude Code 自己的职责。
 */
export async function readRegistry(
  dir: string = SESSIONS_DIR,
  nowMs: number = Date.now(),
): Promise<RegistrySnapshot> {
  let files: string[]
  try {
    files = await readdir(dir)
  } catch (err) {
    const code = (err as NodeJS.ErrnoException).code
    // 目录还没被创建出来是正常情况（Claude Code 尚未启动过）
    if (code === 'ENOENT') return { sessions: [], skipped: 0 }
    throw err
  }

  const sessions: RegistrySession[] = []
  let skipped = 0

  for (const file of files) {
    if (!PID_FILE_RE.test(file)) continue
    try {
      const text = await readFile(join(dir, file), 'utf8')
      const session = toSession(JSON.parse(text), nowMs)
      if (session === null) {
        skipped += 1
        continue
      }
      sessions.push(session)
    } catch {
      // torn read / 文件刚被删 / 权限——都跳过，下一轮自然会重试
      skipped += 1
    }
  }

  // 按 startedAt 升序：先开的排前面。板子据此保证页序稳定，
  // 绝不按活跃度排序——那会让页面在用户手指底下重排。
  const ordered = [...sessions].sort((a, b) => a.startedAtMs - b.startedAtMs)
  return { sessions: ordered, skipped }
}

/** 进入轮播环的会话：活着且近期有活动。 */
export function activeOnly(
  snapshot: RegistrySnapshot,
): readonly RegistrySession[] {
  return snapshot.sessions.filter((s) => s.liveness === 'active')
}
