# Clawd Box

把一块 4 寸触摸屏做成 Claude Code 的状态面板。谁在跑、跑到哪、还剩多少额度，
抬眼就能看见，不用切窗口。

> **非官方个人项目**，与 Anthropic 无隶属或背书关系。
> "Claude" / "Clawd" 是 Anthropic 的商标，角色形象归 Anthropic 所有；
> 本仓库的 MIT 许可只覆盖源代码。详见 [LICENSE](LICENSE)。

---

## 它做什么

- **用动作而不是图标表达状态**：打碟（在干活）、跳跃庆祝（干完了）、
  举灯泡（等你输入）、发呆、睡觉
- 多个会话时在**活跃会话之间轮播**；只有一个在干活时锁定它
- 顶部一排圆点是 subagent（实心=完成，空心=进行中）
- 底部常驻 5 小时 / 7 天额度条与重置倒计时
- 完成、等你输入、额度告急三种提示音，实时合成，不占 flash

## 硬件

微雪 **ESP32-S3-Touch-LCD-4B**（86 盒形态，4″ 480×480 电容触摸）。
实测配置：ESP32-S3 rev v0.2、16 MB flash、8 MB octal PSRAM、
ST7701 RGB 屏、GT911 触摸、ES8311 音频、TCA9554 IO 扩展。

完整引脚表与实测过程见 [docs/pinout.md](docs/pinout.md)。

---

## 最重要的一条设计约束：不给账号添任何风险

**整个项目不向 `api.anthropic.com` 发出任何一个新增请求。** 服务端无从知道
这个装置存在。数据只有两个来源，都是纯本地读取：

| 来源 | 拿到什么 | 为什么安全 |
|---|---|---|
| `statusLine` 的 `rate_limits` | 5 小时 / 7 天额度真实值 | Claude Code 自己解析响应头后塞给 statusLine 的内存值，我们只读，不触发任何 I/O |
| `{"type":"command","async":true}` 钩子 | 会话开始/结束、等待输入、subagent 起止 | `async` 为真时钩子立即返回，永不阻塞对话 |

**明确不做的**（都能拿到数据，但都越线）：

- ❌ 轮询 `/api/oauth/usage` —— 真实客户端的三处调用全部由用户动作触发，**没有任何定时器**，轮询是客户端不存在的请求形态
- ❌ 从 Keychain 取 OAuth token 自发探测请求 —— 在官方客户端之外使用凭证
- ❌ 原生 `{"type":"http"}` 钩子 —— 它是 `await` 的，**同步阻塞对话**，默认超时 600 秒
- ❌ MITM 代理、无头浏览器抓网页端

取舍是真实存在的：按模型细分的额度（比如 Fable）只存在于 `/api/oauth/usage`
的响应体里，所以**我们显示不了**，界面上就留 `--`。宁可少显示一项，
也不为它发一个请求。

---

## 架构：厚板子，薄主机

Mac 只做三件必须在 Mac 上做的事——读会话注册表、校验进程存活、
接住钩子和 statusLine 的 stdin——然后近乎原样转发。

**其余全在板子上**：会话表增删改、计时、状态机与仲裁、动画选择、
页面路由与轮播、渲染、断线降级。

```
Claude Code ──钩子/statusLine──▶ 守护进程(bun) ──UART 921600──▶ 板子
                                      │                          │
                                 只做转发                   全部业务逻辑
```

协议是换行分隔的 JSON，**状态替换而非事件累积**——每帧整行覆盖该会话的状态，
所以板子上内存是 `O(会话数 × 定长)`，结构上不可能增长：

```jsonc
{"e":"session","id":"dfc3f9ef","name":"weixue","status":"busy"}
{"e":"turn_end","id":"dfc3f9ef"}
{"e":"limits","h5":68.2,"h5r":1787160000,"w7":41.0,"w7r":1787600000}
```

---

## 上手

需要 [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/) 和
[bun](https://bun.sh)。

```bash
# 1) 固件
cd firmware
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem* flash

# 2) 主机守护进程（注册为开机自启）
./host/launchd/install.sh

# 3) 把钩子与 statusLine 接进 Claude Code
#    把 host/bin/hook.ts 注册到 UserPromptSubmit / Stop / Notification /
#    SubagentStart / SubagentStop，全部用 {"type":"command","async":true}；
#    把 host/bin/statusline.ts 设为 statusLine 命令。
```

板子有两个 USB 口，**插哪个都能收数据**（日志两个口都输出，
数据两路都读）。守护进程日志在 `/tmp/clawdbox-daemon.log`。

---

## 开发

两个不需要硬件的工具，都是为了不"盲改"：

```bash
# 精灵动效离线预览：同一份 clawd.c 编到本机，直接出图
cd firmware
clang -O2 -I main -o /tmp/prev ../tools/preview/preview.c main/sprite/clawd.c -lm
/tmp/prev /tmp/out.raw        # 输出 "宽 高\n" + RGB888

# 轮播/抢占逻辑的确定性测试（时间是入参，完全可控）
./tools/logic-test/run.sh

# 主机侧单元测试
cd host && bun test
```

离线预览是这个项目最值钱的一件工具。在有它之前，每版动效都是
"改完烧进去、问别人好不好看"——等于闭着眼睛画。

---

## 已知待办

- `firmware/main/sprite/clawd.c` 有 1400 多行，超出了本项目自己定的
  800 行上限。它内部分节清楚（几何 / 光栅化 / 变换 / 道具 / 动作曲线 /
  对外接口），按概念可以切成 3 个文件，但**必须是纯重构**——
  验收标准是 `tools/preview` 的输出逐字节不变。
- DJ 态单帧约 50 ms（20 fps），其余状态 10~28 ms。瓶颈在道具的图元数量。
  想再快就得上双帧缓冲，或者合并唱片那几层椭圆。

## 文档

- [docs/pinout.md](docs/pinout.md) —— 引脚表、外设地址、分区表，全部实测复核
- [docs/gotchas.md](docs/gotchas.md) —— 踩过的坑，每条都有现象、根因和验证方法

`gotchas.md` 值得先读。里面有几条是花了很久才定位的，比如
"日志能出来不等于数据能进去"、"UART 中断优先级不抬到 LEVEL3 就会整段丢帧"、
"抗锯齿混色放 PSRAM 上每个像素撞一次 cache miss"。
