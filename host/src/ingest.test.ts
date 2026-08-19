import { describe, expect, test } from 'bun:test'
import { parseIngestLine } from './ingest.ts'

const hook = (over: Record<string, unknown>) =>
  JSON.stringify({
    k: 'hook',
    payload: {
      hook_event_name: 'Stop',
      session_id: 'dfc3f9ef-5ab0-4ad9-8b72-b0539f602dd4',
      transcript_path: '/x/y.jsonl',
      cwd: '/x',
      ...over,
    },
  })

describe('parseIngestLine —— statusline', () => {
  test('解析出 session_id 并原样保留载荷', () => {
    const msg = parseIngestLine(
      JSON.stringify({
        k: 'statusline',
        payload: { session_id: 'abc-123', rate_limits: { five_hour: { used_percentage: 5 } } },
      }),
    )
    expect(msg?.kind).toBe('statusline')
    if (msg?.kind !== 'statusline') throw new Error('unreachable')
    expect(msg.sessionId).toBe('abc-123')
    expect(msg.payload).toMatchObject({ rate_limits: {} })
  })

  test('缺 session_id 时丢弃', () => {
    expect(parseIngestLine(JSON.stringify({ k: 'statusline', payload: {} }))).toBeNull()
  })
})

describe('parseIngestLine —— hook', () => {
  test('主线程事件：agent_id 为 null', () => {
    const msg = parseIngestLine(hook({}))
    expect(msg?.kind).toBe('hook')
    if (msg?.kind !== 'hook') throw new Error('unreachable')
    expect(msg.event).toBe('Stop')
    expect(msg.agentId).toBeNull()
  })

  test('subagent 事件：agent_id 存在 —— 这是区分主线程与子代理的唯一依据', () => {
    const msg = parseIngestLine(
      hook({
        hook_event_name: 'SubagentStop',
        agent_id: 'a975331e70d6f971f',
        agent_type: 'Explore',
      }),
    )
    if (msg?.kind !== 'hook') throw new Error('unreachable')
    expect(msg.agentId).toBe('a975331e70d6f971f')
    expect(msg.agentType).toBe('Explore')
  })

  test('Notification 的 notification_type 被提取', () => {
    const msg = parseIngestLine(
      hook({ hook_event_name: 'Notification', notification_type: 'idle_prompt' }),
    )
    if (msg?.kind !== 'hook') throw new Error('unreachable')
    expect(msg.notificationType).toBe('idle_prompt')
  })

  test('空字符串字段归一为 null', () => {
    const msg = parseIngestLine(hook({ agent_id: '', agent_type: '' }))
    if (msg?.kind !== 'hook') throw new Error('unreachable')
    expect(msg.agentId).toBeNull()
    expect(msg.agentType).toBeNull()
  })

  test('缺 hook_event_name 时丢弃', () => {
    const msg = parseIngestLine(
      JSON.stringify({ k: 'hook', payload: { session_id: 'x' } }),
    )
    expect(msg).toBeNull()
  })
})

describe('parseIngestLine —— 垃圾输入', () => {
  test.each([
    ['空串', ''],
    ['半截 JSON', '{"k":"hook","pay'],
    ['纯文本', 'hello world'],
    ['数组', '[1,2,3]'],
    ['null', 'null'],
    ['未知 kind', '{"k":"whatever","payload":{}}'],
    ['缺 payload', '{"k":"hook"}'],
    ['payload 非对象', '{"k":"hook","payload":"x"}'],
  ])('%s → null，不抛错', (_label, line) => {
    expect(parseIngestLine(line)).toBeNull()
  })
})
