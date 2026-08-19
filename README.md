# Clawd Box

把 Claude Code 的运行状态显示在一块 **Waveshare ESP32-S3-Touch-LCD-4B**（4″ 480×480 触摸屏，86 盒形态）上。

Clawd 小精灵用**动作**表达主 agent 的状态——干活、干完、等你输入是三种明显不同的动作，不是换图标。subagent 用顶部一排圆点（实心=完成，空心=进行中）。底部常驻 5 小时限额、周限额，以及按模型细分的额度（如 Fable）。

---

## 账号安全：零额外网络请求

这是本项目的第一约束。整个系统**不会向 `api.anthropic.com` 发出任何一个新增请求**，服务端无从知道这个装置存在。

只用两个数据源，都是纯本地读取或官方支持的客户端功能：

| 数据 | 来源 | 为什么安全 |
|---|---|---|
| 5 小时 / 周限额 | `statusLine` 命令的 stdin | Claude Code 从**它自己那次对话的响应头**里解析好放在内存，我们只是读出来。`getRawUtilization()` 是个裸 getter，全树仅 3 处赋值，全在响应头解析路径上。 |
| 会话状态 / subagent | `~/.claude/sessions/*.json` + `async` 钩子 | 只读本地文件；钩子声明 `"async": true`，Claude Code 立即返回，永不阻塞对话。 |
| 按模型细分的额度 | `~/.claude.json` 的 `cachedUsageUtilization` | CLI 自己在用户打开 `/usage` 时写的缓存，我们只读。新鲜度如实标注。 |

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
