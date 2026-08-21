#!/usr/bin/env bun
/**
 * 统一的钩子入口。一个脚本按 hook_event_name 分发，避免注册多个不同命令。
 *
 * **必须在 settings.json 里声明 `"async": true`**，Claude Code 会立即
 * `return { status: 0, backgrounded: true }`，永不阻塞对话。
 * （用声明式而不是靠 stdout 打 `{"async":true}`——后者有竞态：
 *  进程太快、在 'data' 事件触发前写了更多内容，就会解析失败并退化成同步阻塞。）
 *
 * 本脚本**不往 stdout 写任何东西**，也永远 exit 0——
 * 非零退出码会在用户终端里冒出错误信息，退出码 2 更会直接阻断操作。
 *
 * 配置示例（~/.claude/settings.json 的 hooks 段）：
 *   { "matcher": "*", "hooks": [{
 *       "type": "command",
 *       "command": "bun run <abs>/host/bin/hook.ts",
 *       "async": true,
 *       "timeout": 5
 *   }]}
 *
 * 需要注册的事件：
 *   UserPromptSubmit · Stop · SubagentStart · SubagentStop
 *   Notification · SessionStart · SessionEnd
 */

import { deliver, readStdinJson } from '../src/deliver.ts'

/** 只转发我们真正会用到的事件，其余直接丢——减少守护进程侧噪声。 */
const FORWARDED_EVENTS: ReadonlySet<string> = new Set([
  'UserPromptSubmit',
  'Stop',
  'SubagentStart',
  'SubagentStop',
  'Notification',
  'SessionStart',
  'SessionEnd',
])

async function main(): Promise<void> {
  const payload = await readStdinJson()
  if (payload === null || typeof payload !== 'object') return

  const event = (payload as Record<string, unknown>).hook_event_name
  if (typeof event !== 'string' || !FORWARDED_EVENTS.has(event)) return

  await deliver({ k: 'hook', payload })
}

// 任何异常都吞掉：钩子失败绝不能影响用户的对话
try {
  await main()
} catch {
  /* 静默 */
}
process.exit(0)
