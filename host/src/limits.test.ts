import { describe, expect, test } from 'bun:test'
import { parseStatusLineLimits } from './limits.ts'

const NOW = 1_787_145_600_000

describe('parseStatusLineLimits', () => {
  test('解析完整的 rate_limits', () => {
    const out = parseStatusLineLimits(
      {
        session_id: 'x',
        rate_limits: {
          five_hour: { used_percentage: 68.2, resets_at: 1787160000 },
          seven_day: { used_percentage: 41.0, resets_at: 1787600000 },
        },
      },
      NOW,
    )
    expect(out).toEqual({
      fiveHour: { pct: 68.2, resetsAt: 1787160000 },
      sevenDay: { pct: 41.0, resetsAt: 1787600000 },
      atMs: NOW,
    })
  })

  test('rate_limits 整个缺失时返回 null —— 这是首次 API 响应前的正常状态', () => {
    expect(parseStatusLineLimits({ session_id: 'x', model: {} }, NOW)).toBeNull()
  })

  test('只有 five_hour 时 seven_day 为 null（两个窗口可独立缺席）', () => {
    const out = parseStatusLineLimits(
      { rate_limits: { five_hour: { used_percentage: 12, resets_at: 1 } } },
      NOW,
    )
    expect(out?.fiveHour).toEqual({ pct: 12, resetsAt: 1 })
    expect(out?.sevenDay).toBeNull()
  })

  test('resets_at 缺失时百分比仍可用', () => {
    const out = parseStatusLineLimits(
      { rate_limits: { five_hour: { used_percentage: 5 } } },
      NOW,
    )
    expect(out?.fiveHour).toEqual({ pct: 5, resetsAt: null })
  })

  test('百分比被夹到 0-100', () => {
    const over = parseStatusLineLimits(
      { rate_limits: { five_hour: { used_percentage: 143.7, resets_at: 1 } } },
      NOW,
    )
    expect(over?.fiveHour?.pct).toBe(100)

    const under = parseStatusLineLimits(
      { rate_limits: { seven_day: { used_percentage: -3, resets_at: 1 } } },
      NOW,
    )
    expect(under?.sevenDay?.pct).toBe(0)
  })

  test('两个窗口都是垃圾数据时返回 null', () => {
    expect(
      parseStatusLineLimits(
        { rate_limits: { five_hour: { used_percentage: 'x' }, seven_day: null } },
        NOW,
      ),
    ).toBeNull()
  })

  test('NaN / Infinity 被拒绝', () => {
    expect(
      parseStatusLineLimits(
        { rate_limits: { five_hour: { used_percentage: Number.POSITIVE_INFINITY } } },
        NOW,
      ),
    ).toBeNull()
  })

  test('非对象输入不会抛错', () => {
    expect(parseStatusLineLimits(null, NOW)).toBeNull()
    expect(parseStatusLineLimits('nope', NOW)).toBeNull()
    expect(parseStatusLineLimits(42, NOW)).toBeNull()
    expect(parseStatusLineLimits(undefined, NOW)).toBeNull()
  })
})
