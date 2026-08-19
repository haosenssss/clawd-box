import { describe, expect, test } from 'bun:test'
import { createDispatcher, SESSION_THROTTLE_MS } from './dispatch.ts'

const T0 = 1_787_145_600_000

function harness() {
  const lines: string[] = []
  const dispatcher = createDispatcher((line) => lines.push(line))
  const frames = () => lines.map((l) => JSON.parse(l) as Record<string, unknown>)
  return { dispatcher, lines, frames }
}

const input = (over: Record<string, unknown> = {}) => ({
  sessionId: 'dfc3f9ef-5ab0-4ad9',
  name: 'weixue',
  status: 'busy' as const,
  updatedAtMs: T0,
  ...over,
})

describe('会话去重', () => {
  test('内容不变时只发一次', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input(), T0)
    dispatcher.session(input(), T0 + 1000)
    dispatcher.session(input(), T0 + 2000)
    expect(frames()).toHaveLength(1)
    expect(dispatcher.stats()).toMatchObject({ sent: 1, deduped: 2 })
  })

  test('签名不含 updatedAtMs —— 时间戳变化不应触发重发', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input({ updatedAtMs: T0 }), T0)
    dispatcher.session(input({ updatedAtMs: T0 + 500 }), T0 + 1000)
    dispatcher.session(input({ updatedAtMs: T0 + 900 }), T0 + 2000)
    expect(frames()).toHaveLength(1)
  })

  test('状态变化会发新帧', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input({ status: 'busy' }), T0)
    dispatcher.session(input({ status: 'waiting' }), T0 + SESSION_THROTTLE_MS + 1)
    expect(frames()).toHaveLength(2)
    expect(frames()[1]).toMatchObject({ status: 'waiting' })
  })

  test('会话名变化会发新帧', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input({ name: 'a' }), T0)
    dispatcher.session(input({ name: 'b' }), T0 + SESSION_THROTTLE_MS + 1)
    expect(frames()).toHaveLength(2)
  })
})

describe('节流', () => {
  test('窗口内的变化被压掉', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input({ status: 'busy' }), T0)
    dispatcher.session(input({ status: 'idle' }), T0 + SESSION_THROTTLE_MS - 1)
    expect(frames()).toHaveLength(1)
    expect(dispatcher.stats().throttled).toBe(1)
  })

  test('窗口外的变化放行', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input({ status: 'busy' }), T0)
    dispatcher.session(input({ status: 'idle' }), T0 + SESSION_THROTTLE_MS)
    expect(frames()).toHaveLength(2)
  })

  test('首帧不受节流影响', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input(), T0)
    expect(frames()).toHaveLength(1)
  })
})

describe('会话消失', () => {
  test('发 session_gone 并清掉去重记录', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input(), T0)
    dispatcher.sessionGone('dfc3f9ef-5ab0-4ad9')
    // 同样内容再来一次——因为记录已清，应当重新发出
    dispatcher.session(input(), T0 + 10_000)
    expect(frames().map((f) => f.e)).toEqual(['session', 'session_gone', 'session'])
  })

  test('id 截断到 8 字符', () => {
    const { dispatcher, frames } = harness()
    dispatcher.sessionGone('dfc3f9ef-5ab0-4ad9-8b72')
    expect(frames()[0]).toMatchObject({ e: 'session_gone', id: 'dfc3f9ef' })
  })
})

describe('reset', () => {
  test('清空后同样内容会重发 —— 板子重连需要全量快照', () => {
    const { dispatcher, frames } = harness()
    dispatcher.session(input(), T0)
    dispatcher.session(input(), T0 + 10_000)
    expect(frames()).toHaveLength(1)

    dispatcher.reset()
    dispatcher.session(input(), T0 + 20_000)
    expect(frames()).toHaveLength(2)
  })
})

describe('边沿事件', () => {
  test('emit 不去重 —— 同一事件连发多次都要送达', () => {
    const { dispatcher, frames } = harness()
    dispatcher.emit({ e: 'turn_end', id: 'abc' })
    dispatcher.emit({ e: 'turn_end', id: 'abc' })
    expect(frames()).toHaveLength(2)
  })

  test('超长帧被拒绝并计数，不写出半个 JSON', () => {
    const { dispatcher, lines } = harness()
    dispatcher.emit({
      e: 'session',
      id: 'abcdefgh',
      name: 'x'.repeat(600),
      status: 'busy',
      u: 0,
    })
    expect(lines).toHaveLength(0)
    expect(dispatcher.stats().oversize).toBe(1)
  })
})
