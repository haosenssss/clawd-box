/**
 * Unix domain socket 服务端，接收两个入口脚本推来的数据：
 *
 *   bin/statusline.ts —— Claude Code 的 statusLine 命令，带 rate_limits
 *   bin/hook.ts       —— 各类钩子事件（async:true，永不阻塞对话）
 *
 * 用 UDS 而不是 HTTP：客户端写一行就退出，没有 TCP 握手开销，
 * 也不会在本机开监听端口。
 *
 * 服务端对客户端的**唯一承诺是不阻塞**——收到就立刻返回，
 * 解析失败静默丢弃。客户端那边是 fire-and-forget，慢一毫秒都会拖累每一轮对话。
 */

import { mkdir, unlink } from 'node:fs/promises'
import { dirname, join } from 'node:path'
import { homedir } from 'node:os'
import net from 'node:net'

export const SOCKET_PATH = join(homedir(), '.clawd-box', 'ingest.sock')

/** 单条消息字节上限，超了直接丢——防止畸形输入撑爆内存。 */
const MAX_MESSAGE_BYTES = 64 * 1024

/** statusLine 传来的完整载荷（我们只关心其中几个字段）。 */
export type StatusLineMessage = {
  readonly kind: 'statusline'
  readonly sessionId: string
  readonly payload: unknown
}

/** 钩子事件。字段名沿用 Claude Code 的 snake_case 原样。 */
export type HookMessage = {
  readonly kind: 'hook'
  readonly event: string
  readonly sessionId: string
  /** 存在即表示来自 subagent；主线程没有这个字段。 */
  readonly agentId: string | null
  readonly agentType: string | null
  /** Notification 事件专有 */
  readonly notificationType: string | null
}

export type IngestMessage = StatusLineMessage | HookMessage

function str(v: unknown): string | null {
  return typeof v === 'string' && v !== '' ? v : null
}

/** 把一行 JSON 解析成内部消息；解析不出来返回 null（静默丢弃）。 */
export function parseIngestLine(line: string): IngestMessage | null {
  let raw: unknown
  try {
    raw = JSON.parse(line)
  } catch {
    return null
  }
  if (typeof raw !== 'object' || raw === null) return null
  const o = raw as Record<string, unknown>

  if (o.k === 'statusline') {
    const payload = o.payload
    if (typeof payload !== 'object' || payload === null) return null
    const sessionId = str((payload as Record<string, unknown>).session_id)
    if (sessionId === null) return null
    return { kind: 'statusline', sessionId, payload }
  }

  if (o.k === 'hook') {
    const payload = o.payload
    if (typeof payload !== 'object' || payload === null) return null
    const p = payload as Record<string, unknown>
    const event = str(p.hook_event_name)
    const sessionId = str(p.session_id)
    if (event === null || sessionId === null) return null
    return {
      kind: 'hook',
      event,
      sessionId,
      agentId: str(p.agent_id),
      agentType: str(p.agent_type),
      notificationType: str(p.notification_type),
    }
  }

  return null
}

export type IngestServer = {
  readonly close: () => Promise<void>
}

/**
 * 起一个 UDS 服务端。每收到一条完整的行就回调 onMessage。
 * onMessage 内部抛错不会影响后续消息。
 */
export async function startIngestServer(
  onMessage: (msg: IngestMessage) => void,
  socketPath: string = SOCKET_PATH,
): Promise<IngestServer> {
  await mkdir(dirname(socketPath), { recursive: true, mode: 0o700 })
  // 上次没干净退出会留下 socket 文件，先清掉
  await unlink(socketPath).catch(() => {})

  const server = net.createServer((conn) => {
    let buffer = ''
    conn.setEncoding('utf8')

    conn.on('data', (chunk: string) => {
      buffer += chunk
      if (buffer.length > MAX_MESSAGE_BYTES) {
        buffer = ''
        conn.destroy()
        return
      }
      let nl = buffer.indexOf('\n')
      while (nl !== -1) {
        const line = buffer.slice(0, nl)
        buffer = buffer.slice(nl + 1)
        const msg = parseIngestLine(line)
        if (msg !== null) {
          try {
            onMessage(msg)
          } catch {
            // 单条消息处理失败不能拖垮服务端
          }
        }
        nl = buffer.indexOf('\n')
      }
    })

    // 客户端写完就跑，连接重置是常态，不当错误处理
    conn.on('error', () => conn.destroy())
  })

  await new Promise<void>((resolve, reject) => {
    server.once('error', reject)
    server.listen(socketPath, () => {
      server.removeListener('error', reject)
      resolve()
    })
  })

  return {
    close: async () => {
      await new Promise<void>((resolve) => server.close(() => resolve()))
      await unlink(socketPath).catch(() => {})
    },
  }
}
