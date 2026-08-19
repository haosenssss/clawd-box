/**
 * 主机 ↔ 板子 的线协议。换行分隔 JSON，115200 baud。
 *
 * 核心设计：**状态替换，不是事件累积。**
 * `session` 帧整行覆盖该会话的状态；其余帧只是修改定长结构的边沿信号。
 * 板子据此维护定长会话表，内存是 O(会话数 × 定长)，结构上不可能增长。
 *
 * 时间一律用 **unix 秒**（不是毫秒）——省字节，且板子有 RTC 可对时。
 */

export const BAUD_RATE = 115_200

/** 单帧字节上限。超限时丢字段，绝不截断 JSON——截断的 JSON 解析失败更糟。 */
export const MAX_FRAME_BYTES = 512

/** 会话名在线上的字节上限（含结尾 NUL 后固件用 32 字节定长缓冲）。 */
export const NAME_MAX_BYTES = 31

/** subagent 类型名字节上限。 */
export const AGENT_TYPE_MAX_BYTES = 15

/** 会话 id / agent id 截断到前 8 字符——足够区分，且定长好处理。 */
export const ID_CHARS = 8

export type SessionStatus = 'busy' | 'waiting' | 'idle'

/** 限额数据的来源，决定板子怎么渲染（实时=正常亮度，缓存=暗色+陈旧点）。 */
export type LimitSource = 'live' | 'cached'

export type Frame =
  /** 握手 + 对时。板子据此校准 RTC。 */
  | { e: 'hello'; ts: number }
  /** 会话状态快照，整行覆盖。u = 该状态最后更新的 unix 秒。 */
  | { e: 'session'; id: string; name: string; status: SessionStatus; u: number }
  /** 会话消失（进程没了 / 超期淘汰）。板子应立即整条清除。 */
  | { e: 'session_gone'; id: string }
  /** UserPromptSubmit —— 新一轮开始。 */
  | { e: 'prompt'; id: string }
  /** Stop —— 一轮结束。板子据此清空该会话的 subagent 圆点。 */
  | { e: 'turn_end'; id: string }
  /** Notification(idle_prompt) —— 在等你输入。触发退避式重复提醒。 */
  | { e: 'idle_prompt'; id: string }
  | { e: 'sub_start'; id: string; aid: string; type: string }
  | { e: 'sub_stop'; id: string; aid: string }
  /** 5 小时 / 7 天限额。pct 为 0-100，r 为重置的 unix 秒。 */
  | {
      e: 'limits'
      h5: number | null
      h5r: number | null
      w7: number | null
      w7r: number | null
      src: LimitSource
      /** 数据年龄（秒）。src='live' 时为 0。 */
      age: number
    }
  /** 按模型细分的周限额（Fable / Opus / Sonnet…）。只可能来自缓存。 */
  /**
   * 上下文窗口占用，**按会话**而非账号级——所以必须带 id。
   * 同样来自 statusLine 载荷，零额外请求。
   */
  | { e: 'ctx'; id: string; pct: number }

/**
 * 按 UTF-8 字节数截断，且不切断多字节字符。
 * 会话名可能含中文，固件那边是定长字节缓冲，切一半会渲染成乱码。
 */
export function truncateUtf8(input: string, maxBytes: number): string {
  const encoder = new TextEncoder()
  if (encoder.encode(input).length <= maxBytes) return input

  // 逐字符累加，超了就停——比二分查找简单，字符串本来就短
  let bytes = 0
  let out = ''
  for (const char of input) {
    const size = encoder.encode(char).length
    if (bytes + size > maxBytes) break
    bytes += size
    out += char
  }
  return out
}

/** 会话/agent id 统一截断到前 8 字符。 */
export function shortId(id: string): string {
  return id.slice(0, ID_CHARS)
}

export type EncodeResult =
  | { ok: true; line: string }
  | { ok: false; reason: string }

/**
 * 编码一帧为一行。超过 MAX_FRAME_BYTES 则拒绝（返回 ok:false），
 * 由调用方决定丢哪个字段后重试——绝不静默截断出半个 JSON。
 */
export function encodeFrame(frame: Frame): EncodeResult {
  let json: string
  try {
    json = JSON.stringify(frame)
  } catch (err) {
    return { ok: false, reason: `序列化失败: ${String(err)}` }
  }

  const size = new TextEncoder().encode(json).length
  if (size > MAX_FRAME_BYTES) {
    return { ok: false, reason: `帧过大 ${size} > ${MAX_FRAME_BYTES}` }
  }
  return { ok: true, line: `${json}\n` }
}

/** 构造 session 帧，自动做 id/名字截断。 */
export function sessionFrame(args: {
  id: string
  name: string
  status: SessionStatus
  updatedAtMs: number
}): Frame {
  return {
    e: 'session',
    id: shortId(args.id),
    name: truncateUtf8(args.name, NAME_MAX_BYTES),
    status: args.status,
    u: Math.floor(args.updatedAtMs / 1000),
  }
}

/** 构造 sub_start 帧，自动做截断。 */
export function subStartFrame(args: {
  sessionId: string
  agentId: string
  agentType: string
}): Frame {
  return {
    e: 'sub_start',
    id: shortId(args.sessionId),
    aid: shortId(args.agentId),
    type: truncateUtf8(args.agentType, AGENT_TYPE_MAX_BYTES),
  }
}
