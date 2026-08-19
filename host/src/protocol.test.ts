import { describe, expect, test } from 'bun:test'
import {
  AGENT_TYPE_MAX_BYTES,
  encodeFrame,
  MAX_FRAME_BYTES,
  NAME_MAX_BYTES,
  sessionFrame,
  shortId,
  subStartFrame,
  truncateUtf8,
  type Frame,
} from './protocol.ts'

const byteLen = (s: string) => new TextEncoder().encode(s).length

describe('truncateUtf8', () => {
  test('短于上限时原样返回', () => {
    expect(truncateUtf8('weixue', 31)).toBe('weixue')
  })

  test('按字节截断纯 ASCII', () => {
    const out = truncateUtf8('a'.repeat(50), 10)
    expect(out).toBe('a'.repeat(10))
  })

  test('中文不会被切成半个字符', () => {
    // 每个汉字 3 字节；上限 8 字节最多放下 2 个字（6 字节）
    const out = truncateUtf8('限额监控面板', 8)
    expect(out).toBe('限额')
    expect(byteLen(out)).toBeLessThanOrEqual(8)
  })

  test('emoji（4 字节）不会被切断', () => {
    const out = truncateUtf8('🦀🦀🦀', 6)
    expect(out).toBe('🦀')
    expect(byteLen(out)).toBeLessThanOrEqual(6)
  })

  test('上限为 0 时返回空串而非崩溃', () => {
    expect(truncateUtf8('abc', 0)).toBe('')
  })
})

describe('shortId', () => {
  test('截到 8 字符', () => {
    expect(shortId('dfc3f9ef-5ab0-4ad9-8b72-b0539f602dd4')).toBe('dfc3f9ef')
  })

  test('短于 8 字符时原样返回', () => {
    expect(shortId('abc')).toBe('abc')
  })
})

describe('encodeFrame', () => {
  test('正常帧以换行结尾且可被重新解析', () => {
    const frame: Frame = { e: 'hello', ts: 1787145593 }
    const res = encodeFrame(frame)
    expect(res.ok).toBe(true)
    if (!res.ok) throw new Error('unreachable')
    expect(res.line.endsWith('\n')).toBe(true)
    expect(JSON.parse(res.line)).toEqual(frame)
  })

  test('超长帧被拒绝，而不是截断成半个 JSON', () => {
    const frame = {
      e: 'session',
      id: 'x',
      name: 'y'.repeat(MAX_FRAME_BYTES),
      status: 'busy',
      u: 0,
    } as unknown as Frame
    const res = encodeFrame(frame)
    expect(res.ok).toBe(false)
    if (res.ok) throw new Error('unreachable')
    expect(res.reason).toContain('帧过大')
  })

  test('典型 session 帧远小于上限', () => {
    const res = encodeFrame(
      sessionFrame({
        id: 'dfc3f9ef-5ab0-4ad9-8b72-b0539f602dd4',
        name: 'weixue-de',
        status: 'busy',
        updatedAtMs: 1787145593103,
      }),
    )
    expect(res.ok).toBe(true)
    if (!res.ok) throw new Error('unreachable')
    expect(byteLen(res.line)).toBeLessThan(120)
  })
})

describe('sessionFrame', () => {
  test('毫秒转成 unix 秒', () => {
    const f = sessionFrame({
      id: 'abcdef012345',
      name: 'n',
      status: 'idle',
      updatedAtMs: 1787145593103,
    })
    expect(f).toMatchObject({ e: 'session', u: 1787145593 })
  })

  test('超长会话名按 NAME_MAX_BYTES 截断', () => {
    const f = sessionFrame({
      id: 'abcdef012345',
      name: 'x'.repeat(200),
      status: 'idle',
      updatedAtMs: 0,
    })
    if (f.e !== 'session') throw new Error('unreachable')
    expect(byteLen(f.name)).toBeLessThanOrEqual(NAME_MAX_BYTES)
  })
})

describe('subStartFrame', () => {
  test('agent 类型名按上限截断，id 截到 8 字符', () => {
    const f = subStartFrame({
      sessionId: 'dfc3f9ef-5ab0-4ad9',
      agentId: 'a975331e70d6f971f',
      agentType: 'general-purpose-with-a-very-long-name',
    })
    if (f.e !== 'sub_start') throw new Error('unreachable')
    expect(f.id).toBe('dfc3f9ef')
    expect(f.aid).toBe('a975331e')
    expect(byteLen(f.type)).toBeLessThanOrEqual(AGENT_TYPE_MAX_BYTES)
  })
})
