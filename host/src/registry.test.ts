import { afterEach, describe, expect, test } from 'bun:test'
import { mkdtemp, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import {
  ACTIVE_WINDOW_MS,
  activeOnly,
  EVICT_AFTER_MS,
  isPidAlive,
  readRegistry,
} from './registry.ts'

const NOW = 1_787_145_600_000
/** 一个几乎不可能存在的 PID，用来模拟已退出的会话。 */
const DEAD_PID = 4_194_300

const dirs: string[] = []

async function fixtureDir(
  files: Record<string, string>,
): Promise<string> {
  const dir = await mkdtemp(join(tmpdir(), 'clawd-reg-'))
  dirs.push(dir)
  for (const [name, content] of Object.entries(files)) {
    await writeFile(join(dir, name), content)
  }
  return dir
}

afterEach(async () => {
  while (dirs.length > 0) {
    const dir = dirs.pop()
    if (dir !== undefined) await rm(dir, { recursive: true, force: true })
  }
})

const entry = (over: Record<string, unknown> = {}) =>
  JSON.stringify({
    pid: process.pid,
    sessionId: 'dfc3f9ef-5ab0-4ad9-8b72-b0539f602dd4',
    cwd: '/Users/someone/AI/weixue',
    name: 'weixue-de',
    status: 'busy',
    startedAt: NOW - 60_000,
    statusUpdatedAt: NOW - 5_000,
    ...over,
  })

describe('isPidAlive', () => {
  test('自身进程算活着', () => {
    expect(isPidAlive(process.pid)).toBe(true)
  })

  test('不存在的 PID 算死了', () => {
    expect(isPidAlive(DEAD_PID)).toBe(false)
  })
})

describe('readRegistry', () => {
  test('目录不存在时返回空快照而不是抛错', async () => {
    const snap = await readRegistry(join(tmpdir(), 'clawd-does-not-exist-xyz'), NOW)
    expect(snap.sessions).toEqual([])
    expect(snap.skipped).toBe(0)
  })

  test('解析正常条目', async () => {
    const dir = await fixtureDir({ [`${process.pid}.json`]: entry() })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions).toHaveLength(1)
    expect(snap.sessions[0]).toMatchObject({
      pid: process.pid,
      name: 'weixue-de',
      status: 'busy',
      liveness: 'active',
    })
  })

  test('只认 <数字>.json —— 别的文件不会被当成 PID', async () => {
    const dir = await fixtureDir({
      [`${process.pid}.json`]: entry(),
      'notes.md': '# hello',
      '2026-03-14_notes.json': entry(),
      'README': 'x',
    })
    const snap = await readRegistry(dir, NOW)
    // 2026-03-14_notes.json 不匹配 ^\d+\.json$，必须被忽略
    expect(snap.sessions).toHaveLength(1)
  })

  test('torn read（半截 JSON）被跳过并计数，不影响其它条目', async () => {
    const dir = await fixtureDir({
      [`${process.pid}.json`]: entry(),
      '999001.json': '{"pid":999001,"sessi',
    })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions).toHaveLength(1)
    expect(snap.skipped).toBe(1)
  })

  test('缺少 status 字段时返回 null —— 对应 BG_SESSIONS 关闭的构建', async () => {
    const raw = JSON.parse(entry()) as Record<string, unknown>
    delete raw.status
    const dir = await fixtureDir({ [`${process.pid}.json`]: JSON.stringify(raw) })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions[0]?.status).toBeNull()
  })

  test('未知 status 值降级为 idle，不抛错', async () => {
    const dir = await fixtureDir({
      [`${process.pid}.json`]: entry({ status: 'some-future-state' }),
    })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions[0]?.status).toBe('idle')
  })

  test('statusUpdatedAt 缺失时退回 updatedAt，再退回 startedAt', async () => {
    const raw = JSON.parse(entry()) as Record<string, unknown>
    delete raw.statusUpdatedAt
    raw.updatedAt = NOW - 7_000
    const dir = await fixtureDir({ [`${process.pid}.json`]: JSON.stringify(raw) })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions[0]?.updatedAtMs).toBe(NOW - 7_000)
  })

  test('name 缺失时用 cwd 目录名兜底', async () => {
    const dir = await fixtureDir({
      [`${process.pid}.json`]: entry({ name: '', cwd: '/Users/someone/AI/hquant' }),
    })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions[0]?.name).toBe('hquant')
  })

  test('按 startedAt 升序排列 —— 保证板子页序稳定', async () => {
    const dir = await fixtureDir({
      '900001.json': entry({ pid: process.pid, name: '后开的', startedAt: NOW - 1_000 }),
      '900002.json': entry({ pid: process.pid, name: '先开的', startedAt: NOW - 900_000 }),
    })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions.map((s) => s.name)).toEqual(['先开的', '后开的'])
  })
})

describe('liveness 分级', () => {
  test('进程已退出 → dead', async () => {
    const dir = await fixtureDir({
      [`${DEAD_PID}.json`]: entry({ pid: DEAD_PID }),
    })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions[0]?.liveness).toBe('dead')
  })

  test('活着但超过活跃窗口 → parked（实测本机有 19.3 天前的停车会话）', async () => {
    const dir = await fixtureDir({
      [`${process.pid}.json`]: entry({
        statusUpdatedAt: NOW - ACTIVE_WINDOW_MS - 1_000,
      }),
    })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions[0]?.liveness).toBe('parked')
  })

  test('活着但超过淘汰阈值 → dead', async () => {
    const dir = await fixtureDir({
      [`${process.pid}.json`]: entry({
        statusUpdatedAt: NOW - EVICT_AFTER_MS - 1_000,
      }),
    })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions[0]?.liveness).toBe('dead')
  })

  test('activeOnly 只放行 active', async () => {
    const dir = await fixtureDir({
      '900001.json': entry({ pid: process.pid, name: '活跃', startedAt: NOW - 10 }),
      '900002.json': entry({
        pid: process.pid,
        name: '停车',
        startedAt: NOW - 20,
        statusUpdatedAt: NOW - ACTIVE_WINDOW_MS - 1,
      }),
      '900003.json': entry({ pid: DEAD_PID, name: '已死', startedAt: NOW - 30 }),
    })
    const snap = await readRegistry(dir, NOW)
    expect(snap.sessions).toHaveLength(3)
    expect(activeOnly(snap).map((s) => s.name)).toEqual(['活跃'])
  })
})
