/**
 * 串口发送端。只写不读（v1 板子是纯显示端）。
 *
 * macOS 上 bun/node 没有原生串口 API，要绕两个实测踩到的坑：
 *
 * **坑一：设备一关，termios 设置就丢。**
 *   `stty -f <port>` 配完即关闭设备，此时参数恢复默认（9600 之类），
 *   随后再打开写入就全是错误波特率。实测板子只收到 250 字节里的 1 字节。
 *   解法：**先用 openSync 拿住 fd**，再让 stty 去配这个"已经打开"的设备，
 *   我们的 fd 一直不放，设置就一直有效。
 *
 * **坑二：刚打开的头几个字节会丢。**
 *   CH343P 在打开瞬间拨动 DTR/RTS，线路稳定前发出的字节收不全。
 *   实测第一帧（最长的 session 帧）必丢。
 *   解法：打开后 flush 并静默 SETTLE_MS 再开始发。
 *
 * 板子拔掉、重烧、休眠都会让写入失败——全部按"断开"处理并退避重连，
 * 绝不让守护进程因为串口问题崩掉。
 */

import { closeSync, createReadStream, openSync, writeSync } from 'node:fs'
import type { ReadStream } from 'node:fs'
import { readdir } from 'node:fs/promises'
import { spawn } from 'node:child_process'
import { BAUD_RATE } from './protocol.ts'

const DEV_DIR = '/dev'
/** CH343P 在 macOS 上枚举成 CDC-ACM，即 cu.usbmodem* */
const PORT_PREFIX = 'cu.usbmodem'

const RECONNECT_MIN_MS = 500
const RECONNECT_MAX_MS = 10_000

/** 打开后的静默期。实测低于 200ms 仍会丢首帧，取 300ms 留余量。 */
const SETTLE_MS = 300

/**
 * 两帧之间的最小间隔。
 *
 * **必须限速。** 一次性连发 6~7 帧（resync 的场景）时板子会丢字节，
 * 收到的是粘连且缺中段的行，例如：
 *     {"e":"session","id":"1e0508b1","name":"aiagent-4f","status,"name":"clawd-hardwar
 * 原因是板子的渲染循环在密集写 PSRAM，会延迟 UART 中断对硬件 FIFO 的排空。
 * 单帧从不出问题，所以不是波特率或线序问题，就是突发。
 *
 * **15ms 不够。** 实测在 15ms 下稳态 40 秒有个位数丢帧，而且丢的总是
 * 突发里靠后的那几帧——排在队尾的帧长期收不到，看起来像某个功能坏了。
 * 把间隔放到 40ms 后短测零丢帧，但长跑仍会漏——板子的 UART 中断会被 RGB 屏的
 * bounce buffer 中断压住 **40ms 以上**，跨两帧攒够一个 128 字节 FIFO 就整体溢出。
 * 板子那边已把 UART 中断抬到 LEVEL3（见 link.c），这里再把间隔放到 120ms：
 * 单帧最长约 100 字节，**一帧永远填不满 FIFO**，于是只要中断在 120ms 内跑过一次
 * 就绝不会溢出。这是不依赖中断时序的硬保证。
 *
 * 代价为零：一帧约 100 字节、每秒最多几帧，40ms 仍支持 25 帧/秒，
 * 远超实际需要。宁可慢一点也不要发出去的是坏帧。
 */
const FRAME_GAP_MS = 120

export type SerialState = 'connected' | 'disconnected'

export type SerialLink = {
  /** 写一行。未连接时静默丢弃（板子重连后会收到完整快照，不需要补发历史）。 */
  readonly send: (line: string) => void
  readonly state: () => SerialState
  readonly port: () => string | null
  readonly close: () => void
}

/** 列出候选串口。返回排序后的绝对路径。 */
export async function listPorts(devDir: string = DEV_DIR): Promise<string[]> {
  try {
    const names = await readdir(devDir)
    return names
      .filter((n) => n.startsWith(PORT_PREFIX))
      .sort()
      .map((n) => `${devDir}/${n}`)
  } catch {
    return []
  }
}

/**
 * 配置串口参数。
 * `-hupcl` 很关键：关闭时不拉低 DTR，避免每次守护进程重启都把板子复位一次。
 */
function configurePort(port: string): Promise<boolean> {
  return new Promise((resolve) => {
    const args = [
      '-f', port,
      String(BAUD_RATE),
      'cs8', '-cstopb', '-parenb',
      '-crtscts', 'clocal', '-hupcl',
      'raw', '-echo',
    ]
    const child = spawn('stty', args, { stdio: 'ignore' })
    child.on('error', () => resolve(false))
    child.on('exit', (code) => resolve(code === 0))
  })
}

export type SerialOptions = {
  /** 指定端口；不给则自动挑第一个 cu.usbmodem* */
  readonly port?: string
  readonly onStateChange?: (state: SerialState, port: string | null) => void
  /**
   * 板子打印的每一行日志。
   *
   * **必须复用同一个 fd 来读**，绝不能另开一个进程去 `cat` 这个串口：
   * CH343P 在每次 open 时都会拨动 DTR/RTS（见 gotchas #2），
   * 那一下毛刺会打掉正在传输的帧——于是"接上监控就开始丢帧"，
   * 测量本身成了故障源，排查会被带进沟里。
   */
  readonly onBoardLine?: (line: string) => void
}

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms))

export function openSerialLink(options: SerialOptions = {}): SerialLink {
  let fd: number | null = null
  let currentPort: string | null = null
  let state: SerialState = 'disconnected'
  let backoffMs = RECONNECT_MIN_MS
  let timer: ReturnType<typeof setTimeout> | null = null
  let closed = false
  let reader: ReadStream | null = null
  let rxBuf = ''

  /* 限速发送队列 */
  const queue: string[] = []
  let draining = false
  /* 上一帧写出的时刻。**必须是全局的**，不能只在一批之内限速——见 drain()。 */
  let lastWriteMs = 0

  const setState = (next: SerialState, port: string | null) => {
    if (state === next && currentPort === port) return
    state = next
    currentPort = port
    options.onStateChange?.(next, port)
  }

  const teardown = () => {
    if (reader !== null) {
      reader.destroy()
      reader = null
    }
    rxBuf = ''
    if (fd !== null) {
      try {
        closeSync(fd)
      } catch {
        /* 设备已拔出时关闭也会失败，忽略 */
      }
      fd = null
    }
  }

  const scheduleReconnect = () => {
    if (closed || timer !== null) return
    timer = setTimeout(() => {
      timer = null
      void connect()
    }, backoffMs)
    backoffMs = Math.min(backoffMs * 2, RECONNECT_MAX_MS)
  }

  const fail = () => {
    teardown()
    setState('disconnected', null)
    scheduleReconnect()
  }

  const connect = async (): Promise<void> => {
    if (closed) return
    teardown()

    const port = options.port ?? (await listPorts())[0] ?? null
    if (port === null) {
      fail()
      return
    }

    try {
      /* 顺序很重要：先拿住 fd，stty 才能配到一个"保持打开"的设备上 */
      fd = openSync(port, 'r+')
    } catch {
      fail()
      return
    }

    if (!(await configurePort(port))) {
      fail()
      return
    }

    /* 等线路稳定，否则首帧必丢 */
    await sleep(SETTLE_MS)
    if (closed || fd === null) return

    /* 复用同一个 fd 读板子的日志——不新开设备，就不会有 DTR/RTS 毛刺 */
    if (options.onBoardLine !== undefined) {
      reader = createReadStream('', { fd, autoClose: false })
      reader.on('error', () => {/* 拔线时读端也会报错，交给写端统一处理 */})
      reader.on('data', (chunk) => {
        rxBuf += chunk.toString('utf8')
        /* 板子日志偶尔会有超长行，截断而不是无限堆积 */
        if (rxBuf.length > 8192) rxBuf = rxBuf.slice(-4096)
        let nl: number
        while ((nl = rxBuf.indexOf('\n')) >= 0) {
          const line = rxBuf.slice(0, nl).replace(/\r$/, '').trim()
          rxBuf = rxBuf.slice(nl + 1)
          if (line !== '') options.onBoardLine?.(line)
        }
      })
    }

    backoffMs = RECONNECT_MIN_MS
    setState('connected', port)
  }

  const writeOne = (line: string): boolean => {
    if (fd === null) return false
    try {
      /*
       * 一次写完，**不要按返回值做重试循环**。
       * 试过"短写就补发剩余部分"，结果 tty 的 writeSync 返回值偏小但数据已全部写出，
       * 补发把尾部重复了一遍，好帧被改成坏帧（实测收到 `"pct":73ct":73}`）。
       */
      writeSync(fd, line)
      return true
    } catch {
      fail()
      return false
    }
  }

  const drain = async (): Promise<void> => {
    if (draining) return
    draining = true
    while (queue.length > 0 && !closed && state === 'connected') {
      /*
       * 按**上一帧的绝对时刻**补足间隔，而不是"批内两帧之间 sleep"。
       *
       * 之前是后者，于是有个隐蔽的漏洞：一批发完最后一帧就直接退出循环、
       * 不留尾部间隔。resync 的 `hello` 单独成一批，紧跟着注册表轮询又推来
       * 一批 session——两批之间零间隔，等于把两帧背靠背怼进串口。
       * 200 字节挤进 128 字节的硬件 FIFO，中间一整段（连同换行符）被吞掉，
       * 收到的就是"两帧粘连、中段缺失"：
       *     {"e":"session",...,"name":"test"","age":454}   ← session 头 + limits 尾
       * 看起来像随机丢字节，其实是批与批之间根本没限速。
       */
      const wait = lastWriteMs + FRAME_GAP_MS - Date.now()
      if (wait > 0) await sleep(wait)
      if (closed || state !== 'connected') break

      const line = queue.shift()
      if (line === undefined) break
      if (!writeOne(line)) break
      lastWriteMs = Date.now()
    }
    draining = false
  }

  void connect()

  return {
    send: (line: string) => {
      if (fd === null || state !== 'connected') return
      /* 队列上限：板子重连后会收到全量快照，积压历史帧没有意义 */
      if (queue.length > 64) queue.shift()
      queue.push(line)
      if (!draining) void drain()
    },
    state: () => state,
    port: () => currentPort,
    close: () => {
      closed = true
      if (timer !== null) clearTimeout(timer)
      teardown()
      setState('disconnected', null)
    },
  }
}
