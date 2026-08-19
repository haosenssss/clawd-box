/**
 * 限额数据的三个来源，全部是**纯本地文件读取或官方支持的客户端功能**，
 * 零额外网络请求、不使用凭证、不产生真实客户端不存在的流量形态。
 *
 *   1. statusLine 载荷（主源，实时）
 *        Claude Code 每次正常对话的响应头里带 anthropic-ratelimit-unified-5h/7d-*，
 *        它自己解析后存内存，statusLine 只是把已到手的数字读出来给我们。
 *        零额外请求——这条路径实测在源码里只有 3 处赋值，全在响应头解析路径上。
 *
 *   2. Claude Desktop 的 plan-usage-history.json（冷启动兜底 + 趋势）
 *        Desktop 自己写的采样文件，我们只读。实测 15 分钟粒度、6 分钟前刚更新。
 *        用于填补 statusLine 在会话首次 API 响应之前 rate_limits 字段整个不存在的空窗。
 *
 * 曾经还有第三个来源：~/.claude.json 的 cachedUsageUtilization（按模型细分，如 Fable）。
 * 已经放弃——那份缓存只在用户手动打开 /usage 时才刷新，实测停在 24 天前，
 * 而响应头和 Desktop 采样里都不存在按模型细分的额度。完整证据见 docs/gotchas.md #11。
 */

import { readFile, stat } from 'node:fs/promises'
import { join } from 'node:path'
import { homedir } from 'node:os'

export const PLAN_USAGE_HISTORY = join(
  homedir(),
  'Library',
  'Application Support',
  'Claude',
  'plan-usage-history.json',
)

export type WindowLimit = {
  /** 0-100 */
  readonly pct: number
  /** unix 秒；null 表示服务端没给 */
  readonly resetsAt: number | null
}

export type LiveLimits = {
  readonly fiveHour: WindowLimit | null
  readonly sevenDay: WindowLimit | null
  /** 这份数据是什么时候拿到的（本机毫秒时钟） */
  readonly atMs: number
}



// ---------------------------------------------------------------------------
// 1. statusLine 载荷
// ---------------------------------------------------------------------------

/**
 * 从 statusLine 的 stdin JSON 里取限额。
 *
 * 注意 `rate_limits` **在会话首次 API 响应之前整个字段不存在**，这是正常情况，
 * 不是错误——返回 null，由调用方走兜底。
 */
export function parseStatusLineLimits(
  payload: unknown,
  nowMs: number = Date.now(),
): LiveLimits | null {
  if (typeof payload !== 'object' || payload === null) return null
  const rl = (payload as Record<string, unknown>).rate_limits
  if (typeof rl !== 'object' || rl === null) return null

  const pick = (key: string): WindowLimit | null => {
    const w = (rl as Record<string, unknown>)[key]
    if (typeof w !== 'object' || w === null) return null
    const o = w as Record<string, unknown>
    const pct = o.used_percentage
    if (typeof pct !== 'number' || !Number.isFinite(pct)) return null
    const resets = o.resets_at
    return {
      pct: clampPct(pct),
      resetsAt: typeof resets === 'number' && Number.isFinite(resets) ? resets : null,
    }
  }

  const fiveHour = pick('five_hour')
  const sevenDay = pick('seven_day')
  if (fiveHour === null && sevenDay === null) return null
  return { fiveHour, sevenDay, atMs: nowMs }
}

/**
 * 上下文窗口占用百分比。与 rate_limits 不同，这个字段**始终存在**
 * （不依赖是否有过 API 响应），所以是会话刚起来时唯一能立刻显示的数字。
 */
export function parseContextPct(payload: unknown): number | null {
  if (typeof payload !== 'object' || payload === null) return null
  const cw = (payload as Record<string, unknown>).context_window
  if (typeof cw !== 'object' || cw === null) return null
  const pct = (cw as Record<string, unknown>).used_percentage
  if (typeof pct !== 'number' || !Number.isFinite(pct)) return null
  return clampPct(pct)
}

function clampPct(value: number): number {
  if (value < 0) return 0
  if (value > 100) return 100
  return value
}

// ---------------------------------------------------------------------------
// mtime 门控的读取缓存：文件没变就不重复解析
// ---------------------------------------------------------------------------

type Cached<T> = { mtimeMs: number; value: T }

function makeMtimeGatedReader<T>(
  path: string,
  parse: (text: string) => T | null,
): () => Promise<T | null> {
  let cache: Cached<T | null> | null = null

  return async () => {
    let mtimeMs: number
    try {
      mtimeMs = (await stat(path)).mtimeMs
    } catch {
      return null // 文件不存在是正常情况（比如没装 Claude Desktop）
    }
    if (cache !== null && cache.mtimeMs === mtimeMs) return cache.value

    try {
      const text = await readFile(path, 'utf8')
      const value = parse(text)
      cache = { mtimeMs, value }
      return value
    } catch {
      // 解析失败不缓存，下次还会重试
      return null
    }
  }
}

// ---------------------------------------------------------------------------
// 2. Claude Desktop 的采样历史（冷启动兜底）
// ---------------------------------------------------------------------------

export type UsageSample = {
  /** 采样时刻（毫秒） */
  readonly atMs: number
  /** 5 小时窗口百分比 */
  readonly fiveHourPct: number | null
  /** 7 天窗口百分比 */
  readonly sevenDayPct: number | null
}

function parsePlanUsageHistory(text: string): UsageSample | null {
  const data = JSON.parse(text) as unknown
  if (typeof data !== 'object' || data === null) return null
  const samples = (data as Record<string, unknown>).samples
  if (!Array.isArray(samples) || samples.length === 0) return null

  const last = samples[samples.length - 1] as Record<string, unknown>
  const atMs = typeof last.t === 'number' ? last.t : NaN
  if (!Number.isFinite(atMs)) return null

  const u = last.u
  const usage = typeof u === 'object' && u !== null ? (u as Record<string, unknown>) : {}
  const num = (v: unknown): number | null =>
    typeof v === 'number' && Number.isFinite(v) ? clampPct(v) : null

  return { atMs, fiveHourPct: num(usage.fh), sevenDayPct: num(usage.sd) }
}

export const readPlanUsageHistory = makeMtimeGatedReader(
  PLAN_USAGE_HISTORY,
  parsePlanUsageHistory,
)
