<div align="center">

<img src="docs/img/dj.gif" width="380" alt="Clawd 在打碟">

# Clawd Box

**一块 4 寸触摸屏，一只会打碟的小家伙，告诉你代码写得怎么样了。**

放在桌上或墙上，抬眼就知道谁在跑、跑完没有、还剩多少额度——不用切窗口。

</div>

---

## 它会做这些事

<table>
<tr>
<td width="25%" align="center"><img src="docs/img/done.gif" width="170" alt="庆祝"><br><b>干完了</b><br><sub>蹦起来，撒彩纸</sub></td>
<td width="25%" align="center"><img src="docs/img/waiting.gif" width="170" alt="等待"><br><b>等你回话</b><br><sub>举着灯泡，抖</sub></td>
<td width="25%" align="center"><img src="docs/img/idle.gif" width="170" alt="发呆"><br><b>闲着</b><br><sub>左右张望，偶尔抽一下</sub></td>
<td width="25%" align="center"><img src="docs/img/sleeping.gif" width="170" alt="睡觉"><br><b>睡着了</b><br><sub>鼻涕泡一鼓一破</sub></td>
</tr>
</table>

**状态是用动作演出来的，不是图标。** 一眼扫过去不用读字，看姿势就知道在干嘛。

其余在屏上的东西：

- 顶部一排圆点 = 子任务（实心=完成，空心=还在跑）
- 底部两根条 = 5 小时 / 7 天额度，带重置倒计时
- 多个项目同时跑时自动轮播；只有一个在忙时锁定它
- 三种提示音，实时合成，不占存储

---

## 快速上手

**需要**：[ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/)、[bun](https://bun.sh)、一块微雪 ESP32-S3-Touch-LCD-4B。

```bash
git clone https://github.com/haosenssss/clawd-box && cd clawd-box
```

**① 烧固件**

```bash
cd firmware
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem* flash
```

板子有两个 USB 口，插哪个都行。

**② 起主机守护进程**

```bash
./host/launchd/install.sh      # 注册开机自启
```

**③ 接进 Claude Code**

在 `~/.claude/settings.json` 里，把这五个钩子指向 `host/bin/hook.ts`，
statusLine 指向 `host/bin/statusline.ts`：

```jsonc
{
  "statusLine": { "type": "command", "command": "bun run <仓库>/host/bin/statusline.ts" },
  "hooks": {
    // 五个事件都用同一个脚本，async 必须为 true——它跑在对话的关键路径上
    "UserPromptSubmit": [{ "hooks": [{ "type": "command", "async": true,
                           "command": "bun run <仓库>/host/bin/hook.ts" }] }]
    // Stop / Notification / SubagentStart / SubagentStop 同上
  }
}
```

插上板子就能看见了。日志在 `/tmp/clawdbox-daemon.log`。

---

## 怎么做的

**逻辑全在板子上，Mac 只负责递话。**

```
Claude Code ──钩子 / 状态栏──▶ 守护进程 ──串口 921600──▶ 板子
                                  │                      │
                             只做转发              会话表 · 状态机 · 动画 · 渲染
```

Mac 侧只做三件必须在 Mac 上做的事：读会话列表、确认进程还活着、接住钩子的输入。
其余——谁在跑、演什么动作、翻到哪一页、怎么画——全在这块 240 MHz 的芯片上。

协议是**状态替换**而不是事件累积：每条消息整个覆盖某个会话的状态。
所以板子上的内存是"会话数 × 定长"，结构上就不会增长，跑一整天也不会胀。

数据来源只有两处，都是本地读取，**不产生任何额外的网络请求**：
Claude Code 自己解析出来的额度数值，以及它的钩子事件。

---

## 硬件

微雪 **ESP32-S3-Touch-LCD-4B**（86 盒形态，4″ 480×480 电容触摸）。

| | |
|---|---|
| 主控 | ESP32-S3 rev v0.2 · 16 MB flash · 8 MB octal PSRAM |
| 屏 | ST7701 RGB 并口 480×480 |
| 触摸 | GT911 |
| 音频 | ES8311 + 功放 |
| 扩展 | TCA9554（屏幕复位/背光/SPI 片选都挂在它后面） |

引脚表、外设地址、分区表见 [docs/pinout.md](docs/pinout.md)，每一项都经本机实测复核。

---

## 开发

两个不需要硬件的工具：

```bash
# 动效离线预览——同一份 clawd.c 编到本机，直接出图。
# README 里的动图就是它生成的，不是重画的示意图。
cd firmware
clang -O2 -I main -o /tmp/prev ../tools/preview/preview.c main/sprite/clawd.c -lm
/tmp/prev /tmp/out.raw

# 轮播/抢占逻辑的确定性测试（时间是入参，不用等真实时钟）
./tools/logic-test/run.sh

# 主机侧单元测试
cd host && bun test
```

离线预览是这个项目最值钱的一件工具。在有它之前，每改一版动效都得烧进板子再问别人
好不好看——等于闭着眼睛画。

[docs/gotchas.md](docs/gotchas.md) 记了一路踩过的坑，每条都有现象、根因和验证方法。
比如"日志能出来不等于数据能进去"、"串口中断优先级不抬高就会整段丢帧"、
"抗锯齿混色放在 PSRAM 上每个像素都撞一次缓存缺失"。

---

## 已知待办

- `firmware/main/sprite/clawd.c` 有 1400 多行，超出本项目自定的 800 行上限。
  内部分节清楚，可以按概念切成三个文件——但**必须是纯重构**，
  验收标准是 `tools/preview` 的输出逐字节不变。
- 打碟那个状态单帧约 50 ms（20 fps），其余状态 10~28 ms。瓶颈在图元数量。

---

<div align="center">
<sub>

非官方个人项目，与 Anthropic 无隶属或背书关系。
"Claude" / "Clawd" 是 Anthropic 的商标，角色形象归 Anthropic 所有。
本仓库以 [MIT](LICENSE) 开源，许可仅覆盖源代码。

</sub>
</div>
