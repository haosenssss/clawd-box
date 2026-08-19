#!/usr/bin/env bun
/**
 * Claude Code 的 statusLine 命令。
 *
 * 做两件事：
 *   1. 把整个载荷投递给守护进程（里面有服务端真实的 rate_limits）
 *   2. 往 stdout 打一行给用户看的精简状态
 *
 * **零网络请求。** rate_limits 是 Claude Code 从它自己那次对话的响应头里
 * 解析好放在内存里的，我们只是读出来——不产生任何指向 api.anthropic.com 的流量。
 *
 * 配置（~/.claude/settings.json）：
 *   "statusLine": { "type": "command", "command": "bun run <abs>/host/bin/statusline.ts" }
 */

import { deliver, readStdinJson } from './send.ts'
import { parseStatusLineLimits, type WindowLimit } from '../src/limits.ts'

/** 超过这个百分比就标记为吃紧 */
const WARN_PCT = 80
const CRIT_PCT = 95

const DIM = '\x1b[2m'
const RESET = '\x1b[0m'
const ORANGE = '\x1b[38;5;173m'
const AMBER = '\x1b[38;5;214m'
const RED = '\x1b[38;5;203m'

function colorFor(pct: number): string {
  if (pct >= CRIT_PCT) return RED
  if (pct >= WARN_PCT) return AMBER
  return ORANGE
}

/** 沿用 Claude Code 官方 formatResetTime 的观感：24h 内显示时刻，超过显示日期。 */
function formatReset(resetsAt: number | null): string {
  if (resetsAt === null) return ''
  const date = new Date(resetsAt * 1000)
  const hoursUntil = (date.getTime() - Date.now()) / 3_600_000
  if (hoursUntil < 0) return ''
  if (hoursUntil > 24) {
    return date.toLocaleDateString('en-US', { month: 'short', day: 'numeric' })
  }
  const minutes = date.getMinutes()
  return date
    .toLocaleTimeString('en-US', {
      hour: 'numeric',
      minute: minutes === 0 ? undefined : '2-digit',
      hour12: true,
    })
    .replace(/\s?([AP])M/i, (_m, p: string) => p.toLowerCase() + 'm')
}

function renderWindow(label: string, limit: WindowLimit | null): string {
  if (limit === null) return `${DIM}${label} —${RESET}`
  const pct = Math.floor(limit.pct)
  const reset = formatReset(limit.resetsAt)
  const tail = reset === '' ? '' : `${DIM}→${reset}${RESET}`
  return `${colorFor(limit.pct)}${label} ${pct}%${RESET}${tail}`
}

function str(v: unknown): string | null {
  return typeof v === 'string' && v !== '' ? v : null
}

async function main(): Promise<void> {
  const payload = await readStdinJson()
  if (payload === null) {
    process.stdout.write('')
    return
  }

  // 先投递，再渲染——投递有 150ms 上限，不会拖慢终端
  await deliver({ k: 'statusline', payload })

  const p = payload as Record<string, unknown>
  const model = p.model as Record<string, unknown> | undefined
  const modelName = str(model?.display_name) ?? '?'
  const sessionName = str(p.session_name)

  const limits = parseStatusLineLimits(payload)
  const parts = [
    sessionName === null ? null : `${DIM}${sessionName}${RESET}`,
    `${DIM}${modelName}${RESET}`,
    renderWindow('5h', limits?.fiveHour ?? null),
    renderWindow('wk', limits?.sevenDay ?? null),
  ].filter((x): x is string => x !== null)

  process.stdout.write(parts.join(`${DIM} · ${RESET}`))
}

await main()
