# Clawd Box

把 Claude Code 的运行状态显示在一块 **Waveshare ESP32-S3-Touch-LCD-4B**（4″ 480×480 触摸屏，86 盒形态）上。

Clawd 小精灵用**动作**表达主 agent 的状态——干活、干完、等你输入是三种明显不同的动作，不是换图标。subagent 用顶部一排圆点（实心=完成，空心=进行中）。底部常驻三条：5 小时限额、周限额、上下文占用。

---

## 账号安全：零额外网络请求

这是本项目的第一约束。整个系统**不会向 `api.anthropic.com` 发出任何一个新增请求**，服务端无从知道这个装置存在。

只用两个数据源，都是纯本地读取或官方支持的客户端功能：

| 数据 | 来源 | 为什么安全 |
|---|---|---|
| 5 小时 / 周限额 | `statusLine` 命令的 stdin | Claude Code 从**它自己那次对话的响应头**里解析好放在内存，我们只是读出来。`getRawUtilization()` 是个裸 getter，全树仅 3 处赋值，全在响应头解析路径上。 |
| 会话状态 / subagent | `~/.claude/sessions/*.json` + `async` 钩子 | 只读本地文件；钩子声明 `"async": true`，Claude Code 立即返回，永不阻塞对话。 |
| 上下文占用 | `statusLine` 的 `context_window` | 同上，也是已经到手的数字。 |

> **按模型细分的额度（Fable）做不了，已放弃。** 响应头里根本没有这种桶
> （`unified-7d-opus` 在二进制里 0 处匹配），Claude Desktop 的采样文件也只有
> 五小时和七天两个字段。唯一有它的是 `~/.claude.json` 里那份只在你手动打开
> `/usage` 时才刷新的缓存——实测停在 24 天前。**一个看起来实时、实际过期三周的
> 数字比空着更糟**，而要它变准就只能自己发请求，那是红线。完整证据见
> `docs/gotchas.md` #11。

**明确排除**的做法：自己调 `/api/oauth/usage`（真实客户端从不轮询该端点）、从 Keychain 取 OAuth token 自发请求、原生 `http` 类型钩子（它是同步阻塞的，默认超时 600 秒）。

详见 `docs/` 与实施方案。

---

## 目录

```
host/        Mac 侧守护进程（bun + TypeScript）
firmware/    ESP-IDF v5.5 固件
docs/        引脚表、踩坑记录
backup/      出厂固件全片备份（不进版本库）
tools/       串口监视器等
```

---

## 快速开始

### 1. 备份出厂固件（**先做这个**）

```bash
uvx esptool --chip esp32s3 --port /dev/cu.usbmodem* --baud 921600 \
  read-flash 0 0x1000000 backup/factory-full-16MB.bin
```

恢复：把 `read-flash` 换成 `write-flash 0 <文件>`。

### 2. 编译烧录固件

```bash
brew install cmake ninja                    # ESP-IDF 的 install.sh 在 macOS 上不带
git clone -b release/v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32s3

cd firmware
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem* flash
```

### 3. 启动守护进程

```bash
cd host && bun install
bun run src/index.ts            # 加 --print 可脱离硬件，只打印帧
```

### 4. 接上 Claude Code

在 `~/.claude/settings.json` 里加（把 `<ABS>` 换成本仓库绝对路径）：

```jsonc
{
  "statusLine": {
    "type": "command",
    "command": "bun run <ABS>/host/bin/statusline.ts"
  },
  "hooks": {
    "UserPromptSubmit": [{ "matcher": "*", "hooks": [
      { "type": "command", "command": "bun run <ABS>/host/bin/hook.ts", "async": true, "timeout": 5 }]}],
    "Stop":           [{ "matcher": "*", "hooks": [
      { "type": "command", "command": "bun run <ABS>/host/bin/hook.ts", "async": true, "timeout": 5 }]}],
    "SubagentStart":  [{ "matcher": "*", "hooks": [
      { "type": "command", "command": "bun run <ABS>/host/bin/hook.ts", "async": true, "timeout": 5 }]}],
    "SubagentStop":   [{ "matcher": "*", "hooks": [
      { "type": "command", "command": "bun run <ABS>/host/bin/hook.ts", "async": true, "timeout": 5 }]}],
    "Notification":   [{ "matcher": "*", "hooks": [
      { "type": "command", "command": "bun run <ABS>/host/bin/hook.ts", "async": true, "timeout": 5 }]}]
  }
}
```

> `"async": true` 不是可选项。没有它，钩子会同步阻塞每一轮对话。

---

## 开发

```bash
cd host
bun test                 # 63 个测试
bunx tsc --noEmit        # 严格类型检查

python3 tools/monitor.py 10 --reset    # 看板子日志
```

---

## 已实现

- 出厂固件备份与完整引脚表（从出货固件的开源板级定义反查，非推测）
- 主机守护进程：注册表轮询、PID 存活校验、限额三源合并、UDS 接收、串口发送
- 固件：AXP2101 → TCA9554 → ST7701 480×480 RGB 面板点亮
- Clawd 参数化渲染器：10 个矩形、四种状态动作、零素材
- 会话表、subagent 记账、状态仲裁、多会话轮播
- subagent 圆点、限额条

## 待做

- 文字渲染（会话名 + 状态词、限额百分比）
- 触摸手势与两个实体按键、管理页
- 退避式重复提醒
- IMU 重力效果、ES8311 音效

---

## 开机自启

```bash
cp host/launchd/com.clawdbox.daemon.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.clawdbox.daemon.plist
```

日志在 `/tmp/clawdbox-daemon.log`，**板子自己的输出也会进这个文件**（前缀 `板 |`）。

> 守护进程用同一个 fd 读板子日志，**不要另开进程去 `cat` 那个串口**：
> CH343P 每次 open 都会拨动 DTR/RTS，那一下毛刺会打掉正在传输的帧——
> 于是"一接上监控就开始丢帧"，测量本身成了故障源。

路径写死在 plist 里，仓库不在 `~/AI/weixue` 时需要一并改。

---

## 操作

| 动作 | 效果 |
|---|---|
| 右滑 / BOOT 长按 1 秒 | 回管理页——**左边永远是管理页**，不管当前停在哪 |
| 左滑 / BOOT 短按 | 下一个会话页 |
| 点按 / BOOT 双击 / PWR 短按 | 确认：静音提醒。**视觉状态照旧**，状态本身消失才算完 |
| 倾斜 / 晃动 | 精灵朝低处偏一点、晃一下弹起。对**静止姿态**取相对值，所以怎么装都居中 |

轮播只在"正在干活"和"等待输入"之间进行，空闲会话不占页。
**恰好一个**会话在干活时直接锁屏在它上面——那一刻你想盯着的就是它。
手动操作后自动轮播暂停 30 秒。

## 提示音

不放音频资源，正弦振荡器 + 包络实时合成。

| 事件 | 音 | 重复 |
|---|---|---|
| 任务完成 | 上行三音 C5-E5-G5 | 只响一次——这是通知不是催促 |
| 等你输入 | 叩门式重复双音 B5 B5 | 0 / 30s / 1m / 2m / 5m，之后每 5 分钟 |
| 限额 >95% | 下行低音 | 跨过阈值时一次 |
