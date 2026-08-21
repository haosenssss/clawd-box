/**
 * 往守护进程的 UDS 投递一条消息。hook.ts 与 statusline.ts 共用。
 *
 * 铁律：**永不阻塞、永不抛错。** 这段代码跑在 Claude Code 的关键路径上
 * （statusLine 每条 assistant 消息触发一次，钩子每轮对话触发多次），
 * 守护进程没开、socket 不存在、写入失败——一律静默放弃。
 * 宁可板子少显示一帧，也不能让用户的对话卡一下。
 */

import net from 'node:net'
import { SOCKET_PATH } from '../src/ingest.ts'

/** 投递超时。超过就放弃——守护进程要么秒回要么就是没在跑。 */
const DELIVER_TIMEOUT_MS = 150

export function deliver(payload: unknown, socketPath: string = SOCKET_PATH): Promise<void> {
  return new Promise((resolve) => {
    let done = false
    const finish = () => {
      if (done) return
      done = true
      resolve()
    }

    let line: string
    try {
      line = `${JSON.stringify(payload)}\n`
    } catch {
      finish()
      return
    }

    const socket = net.connect(socketPath)
    const timer = setTimeout(() => {
      socket.destroy()
      finish()
    }, DELIVER_TIMEOUT_MS)

    const cleanup = () => {
      clearTimeout(timer)
      socket.destroy()
      finish()
    }

    socket.on('connect', () => socket.end(line))
    socket.on('close', cleanup)
    socket.on('error', cleanup)
  })
}

/** 读完 stdin 并解析为 JSON；失败返回 null。 */
export async function readStdinJson(): Promise<unknown | null> {
  try {
    const chunks: Buffer[] = []
    for await (const chunk of process.stdin) {
      chunks.push(chunk as Buffer)
    }
    const text = Buffer.concat(chunks).toString('utf8').trim()
    if (text === '') return null
    return JSON.parse(text) as unknown
  } catch {
    return null
  }
}
