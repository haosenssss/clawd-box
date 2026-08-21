<div align="center">

<img src="assets/hero.png" width="720" alt="Clawd Box">

# Clawd Box

桌上放一块 4 寸屏，显示 Claude Code 现在在干什么，
省得开着好几个终端的时候挨个切过去看哪个跑完了。

</div>

---

## 屏幕上是什么

<img src="assets/screen.png" width="290" align="right" alt="板子的真实画面">

右边这张是板子实际显示的画面，480×480 原样导出，上面那张宣传图是概念渲染，
外壳比实物精致不少。

顶上那排点对应当前项目派出去的子任务，实心的已经结束，空心的还在跑。中间是
Clawd，它的动作代表项目处于哪种状态，下一节单独说。名字底下那行是它这会儿在
做的事，文案从一组动词里随机取。最下面三根条依次是 5 小时额度、7 天额度和上下文
用量，右边各自跟着重置倒计时。

同时开着几个项目的时候画面会在它们之间轮播，如果只有一个在忙就一直停在它上面，
跑完的那一下还会响一声提示音。

<br clear="right">

## 五种状态

<table>
<tr>
<td width="20%" align="center"><img src="assets/dj.gif" width="150" alt=""></td>
<td width="20%" align="center"><img src="assets/done.gif" width="150" alt=""></td>
<td width="20%" align="center"><img src="assets/waiting.gif" width="150" alt=""></td>
<td width="20%" align="center"><img src="assets/idle.gif" width="150" alt=""></td>
<td width="20%" align="center"><img src="assets/sleeping.gif" width="150" alt=""></td>
</tr>
<tr>
<td align="center"><b>正在跑</b><br><sub>戴着耳机打碟</sub></td>
<td align="center"><b>跑完了</b><br><sub>蹦起来撒彩纸</sub></td>
<td align="center"><b>等你回话</b><br><sub>举着灯泡发抖</sub></td>
<td align="center"><b>闲着</b><br><sub>左右张望</sub></td>
<td align="center"><b>没活干</b><br><sub>睡着，冒鼻涕泡</sub></td>
</tr>
</table>

这几个动作各自对应一种项目状态，看姿势就能分辨出来，不用去读屏幕上的字。

## 上手

要装 [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/) 和 [bun](https://bun.sh)，硬件是微雪 ESP32-S3-Touch-LCD-4B。

```bash
git clone https://github.com/haosenssss/clawd-box && cd clawd-box
```

烧固件：

```bash
cd firmware
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem* flash
```

板子上有两个 USB 口，接哪一个都能收到数据。

守护进程装一次就行，之后每次开机会自动启动：

```bash
./host/launchd/install.sh
```

最后在 `~/.claude/settings.json` 里把钩子和状态栏指过来：

```jsonc
{
  "statusLine": {
    "type": "command",
    "command": "bun run /path/to/clawd-box/host/bin/statusline.ts"
  },
  "hooks": {
    // UserPromptSubmit / Stop / Notification / SubagentStart / SubagentStop
    // 五个事件都指向同一个脚本。async 要写 true，它跑在对话的关键路径上。
    "Stop": [{ "hooks": [{
      "type": "command",
      "async": true,
      "command": "bun run /path/to/clawd-box/host/bin/hook.ts"
    }] }]
  }
}
```

接上板子之后画面就会跟着变，守护进程的日志写在 `/tmp/clawdbox-daemon.log`。

## 怎么搭的

<div align="center">
<img src="assets/arch.svg" width="760" alt="架构">
</div>

Mac 这边只负责读会话列表、确认进程还活着、接住钩子送来的输入，然后把消息递过去，
剩下的逻辑都在板子上跑，包括会话状态、动画选择、翻页和渲染。

消息是状态替换式的，每条整个覆盖掉某个会话的状态，不做事件累积，所以板子上占用的
内存等于会话数乘以定长，连着跑一整天也不会往上涨。

数据来源只有两处，一是 Claude Code 自己算好的额度数值，二是它的钩子事件，
两者都在本地读取，不会额外产生网络请求。

## 硬件

| | |
|---|---|
| 主控 | ESP32-S3 · 16 MB flash · 8 MB PSRAM |
| 屏 | ST7701 RGB 480×480 |
| 触摸 | GT911 |
| 音频 | ES8311 + 功放 |
| 扩展 | TCA9554（屏幕复位、背光、SPI 片选都在它后面） |

## 开发

有两个工具不接板子就能跑。

第一个是动效预览，把固件里那份 `clawd.c` 直接编到本机出图，README 里的动图和
截图都是它生成的：

```bash
cd firmware
clang -O2 -I main -o /tmp/prev ../tools/preview/preview.c main/sprite/clawd.c -lm
/tmp/prev /tmp/out.raw
```

第二个是轮播和抢占逻辑的测试，时间作为函数入参传进去，不用等真实时钟走：

```bash
./tools/logic-test/run.sh
```

主机侧：

```bash
cd host && bun test
```

---

非官方个人项目。"Claude" 和 "Clawd" 是 Anthropic 的商标，形象归他们所有，本仓库以 [MIT](LICENSE) 开源，只覆盖代码。
