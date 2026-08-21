# ESP32-S3-Touch-LCD-4B 引脚表与硬件事实

> Phase 0 交付物。**所有内容均来自实测或出货固件的开源板级定义，无一处推测。**
> 来源：① ROM loader 直接探测本机硬件 ② esptool 5.3.1 特性检测 ③ 出厂固件全片备份的分区表与 app 描述符 ④ [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) `main/boards/waveshare/esp32-s3-touch-lcd-4b/`（即本板出货固件的源码）

> **关于第四项来源**：本仓库不转发 xiaozhi-esp32 的源码文件（那是别人的代码，
> 有自己的许可）。需要核对原始定义时请直接看上游：
>
> ```
> git clone https://github.com/78/xiaozhi-esp32
> ls xiaozhi-esp32/main/boards/waveshare/esp32-s3-touch-lcd-4b/
> #   config.h        引脚宏定义
> #   config.json     板型与 flash 配置
> #   *.cc            ST7701 初始化序列、TCA9554 位分配、ES8311 接线
> ```
>
> 下面表里的每一项都经本机实测复核过（I2C 扫描、寄存器读回、逐个外设点亮），
> 不是照抄——出货固件与实物存在出入的地方已在正文标注。

---

## 1. 芯片与存储（实测）

| 项目 | 值 | 来源 |
|---|---|---|
| 芯片 | ESP32-S3 (QFN56) **revision v0.2** | esptool + eFuse BLOCK1 |
| 特性 | Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz | esptool |
| **PSRAM** | **内置 8 MB，octal，80MHz** ✅ | 上机实测（下详） |
| **Flash** | **16 MB**，JEDEC `0x184046`（manuf `0x46`, dev `0x4018`） | SPI RDID (0x9F) |
| Flash 模式 | eFuse 设定 **quad（4 数据线）**，3.3V | esptool |
| 晶振 | 40 MHz | esptool |
| MAC | `94:A9:90:CD:29:70` | eFuse |
| USB 桥 | **CH343P** (`1a86:55d3`)，CDC-ACM | USB 描述符 |
| 串口 | `/dev/cu.usbmodem5B910325971` @ 最高 921600 实测可用 | 实测 |

> ⚠️ 早先我按旧版 eFuse 对照表把 `PKG_VERSION=0` 读成"无内置 PSRAM"，**该判断作废**。

**PSRAM 上机实测明细**（Phase 0 探测固件启动日志）：

```
octal_psram: vendor id 0x0d (AP)   dev id 0x02 (generation 3)
octal_psram: density 0x03 (64 Mbit)  good-die Pass  VCC 3V
octal_psram: Readlatency 10 cycles@Fixed   BurstLen 32 Byte
esp_psram: Found 8MB PSRAM device    Speed: 80MHz
esp_psram: SPI SRAM memory test OK
→ 运行时可用 8189 KB PSRAM / 371 KB 内部 RAM
```

必须的 sdkconfig：`CONFIG_SPIRAM_MODE_OCT=y` + `CONFIG_SPIRAM_SPEED_80M=y`。
（帧缓冲 480×480×2×2 = 900 KB，相对 8189 KB 余量极大。）

控制台 UART 落在 **GPIO43(TX)/44(RX)**，经 CH343P 桥接出来。

## 2. 出厂固件（已全片备份）

`backup/factory-full-16MB.bin` · 16,777,216 B · SHA256 `1c45c67cc90dbb575683a3663dd72510eb89d34f19f1069cd091a762cc9ed785`

| 分区 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | 0x009000 | 24 KB |
| otadata | data/ota | 0x00F000 | 8 KB |
| phy_init | data/phy | 0x011000 | 4 KB |
| model | data/spiffs | 0x012000 | 0.93 MB（唤醒词模型） |
| factory | app | 0x100000 | 5.00 MB |
| ota_0 | app | 0x600000 | 6.00 MB |
| storage | data/spiffs | 0xC00000 | 3.96 MB |

| 分区 | project_name | version | 编译时间 | IDF |
|---|---|---|---|---|
| factory | phone_s3_box_3 | v0.4.2-92-g5c6be6c-dirty | 2025-12-20 14:03 | v5.5.1-dirty |
| ota_0 | **xiaozhi** | **1.8.5** | 2026-01-23 14:16 | v5.5.2-249 |

设备名 `Xiaozhi-2971`，OTA 端点 `https://api.tenclass.net/xiaozhi/ota/`。

**恢复出厂**：`uvx esptool --chip esp32s3 --port <PORT> --baud 921600 write-flash 0 backup/factory-full-16MB.bin`

---

## 3. 引脚表

### 3.1 I2C —— 一条总线挂全部

```
I2C_NUM_0    SDA = GPIO47    SCL = GPIO48
             glitch_ignore_cnt = 7, 内部上拉使能
```

**全部地址已上机扫描确认**（Phase 0 探测固件，2026-08-19）：

| 器件 | **实测地址** | 用途 | 出货固件是否使用 |
|---|---|---|---|
| **GT911** | **`0x5D`**（或 `0x14`）⚠️ | 电容触摸 | ✅ |
| ES8311 | `0x18` | 音频 codec | ✅ |
| TCA9554 | `0x20` | IO 扩展 | ✅ |
| AXP2101 | `0x34` | 电源管理 | ✅ |
| ES7210 | `0x40` | 回声消除参考 | ✅ |
| **PCF85063** | **`0x51`** ✅ | RTC | ❌ 出货固件没用 |
| **QMI8658** | **`0x6B`** ⚠️ | 六轴 IMU | ❌ 出货固件没用 |

> ⚠️ **两处与常见默认值不同，照抄文档会连不上**：
> - GT911 的地址**由复位时 INT 引脚电平决定**，`0x5D` 和 `0x14` 都可能出现。
>   实测当前固件下是 `0x5D`。**不要写死**，用 `i2c_master_probe()` 逐个探——
>   详见 gotchas #6。
> - QMI8658 用的是**备选地址 `0x6B`**，不是默认的 `0x6A`
>
> **重力效果要自己驱动 QMI8658，时钟要自己驱动 PCF85063** —— xiaozhi 里这两个器件一行代码都没有，但器件确实在总线上。

### 3.2 显示 —— ST7701，480×480，RGB 并口

```
VSYNC = GPIO3     HSYNC = GPIO46    DE = GPIO17    PCLK = GPIO9
DISP  = NC        RST   = NC（走扩展器 EXP5）

DATA0..15 = GPIO 40, 41, 42,  2,  1, 21,  8, 18,
                    45, 38, 39, 10, 11, 12, 13, 14

背光 = GPIO4，**输出逻辑反相**（DISPLAY_BACKLIGHT_OUTPUT_INVERT = true）
```

**ST7701 寄存器初始化走 3-wire SPI，三根脚全在 TCA9554 上**：

```
CS  = EXP0        SDA = EXP1        SCL = EXP2
```

**RGB 时序（出货固件实跑值，直接照用）**：

```c
.pclk_hz            = 16 * 1000 * 1000,
.h_res = 480,  .v_res = 480,
.hsync_pulse_width  = 10,  .hsync_back_porch = 10,  .hsync_front_porch = 20,
.vsync_pulse_width  = 10,  .vsync_back_porch = 10,  .vsync_front_porch = 10,
.flags.pclk_active_neg = false,

.num_fbs             = 2,           // PSRAM 双缓冲
.bounce_buffer_size_px = 480 * 20,
.dma_burst_size      = 64,
.flags.fb_in_psram   = 1,

panel_config.rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB;
panel_config.bits_per_pixel  = 18;
panel_config.reset_gpio_num  = GPIO_NUM_NC;
```

方向：`SWAP_XY=false, MIRROR_X=false, MIRROR_Y=false`，偏移 0/0。

### 3.3 TCA9554 位分配与上电序列

| 位 | 方向 | 用途 |
|---|---|---|
| EXP0 | 输出 | LCD SPI **CS** |
| EXP1 | 输出 | LCD SPI **SDA** |
| EXP2 | 输出 | LCD SPI **SCL** |
| EXP3 | 输出，置 **1** | 使能（具体含义待原理图确认） |
| EXP4 | **输入** | 用途未知 |
| EXP5 | 输出 | **LCD 复位**（低脉冲） |
| EXP6 | 先输出置 0，再转**输入** | 用途未知 |

**必须按此顺序，延时不能省**（出货固件原样）：

```c
esp_io_expander_new_i2c_tca9554(bus, 0x20, &io_expander);
esp_io_expander_set_dir  (io, PIN_3 | PIN_5 | PIN_6, IO_EXPANDER_OUTPUT);
esp_io_expander_set_level(io, PIN_3, 1);
esp_io_expander_set_level(io, PIN_6, 0);
vTaskDelay(pdMS_TO_TICKS(200));
esp_io_expander_set_level(io, PIN_5, 0);      // 复位拉低
vTaskDelay(pdMS_TO_TICKS(200));
esp_io_expander_set_level(io, PIN_5, 1);      // 释放复位
vTaskDelay(pdMS_TO_TICKS(200));
esp_io_expander_set_dir  (io, PIN_4 | PIN_6, IO_EXPANDER_INPUT);
```

### 3.4 音频 —— ES8311 + ES7210

```
I2S   MCLK = GPIO5    WS/LRCK = GPIO7    BCLK = GPIO16
      DIN  = GPIO15   DOUT    = GPIO6
PA 使能 = 无（GPIO_NUM_NC）
采样率  = 输入 24000 / 输出 24000
ES7210 作为 AEC 参考（AUDIO_INPUT_REFERENCE = true）
喇叭    = MX1.25 2P，8Ω 2W
```

### 3.5 按键

| 键 | 连接 | 可读性 |
|---|---|---|
| **BOOT** | **GPIO0** | 运行时自由读；出货固件已用单击/双击 |
| **PWR** | AXP2101 PWRON | 长按 **4s 硬件关机**（PMU 接管）；短按需读 AXP2101 IRQ 状态寄存器 —— **Phase 0 待验证** |

### 3.6 AXP2101 电源配置（出货固件设定，照抄）

```c
WriteReg(0x22, 0b110);  // PWRON > OFFLEVEL 作为关机源，使能
WriteReg(0x27, 0x10);   // 长按 4s 关机
WriteReg(0x80, 0x01);   // 关闭除 DC1 外全部 DCDC
WriteReg(0x90, 0x00);   // 关闭全部 LDO
WriteReg(0x91, 0x00);
WriteReg(0x82, (3300-1500)/100);  // DC1 = 3.3V（主轨）
WriteReg(0x92, (3300- 500)/100);  // ALDO1 = 3.3V
WriteReg(0x90, 0x01);   // 使能 ALDO1（麦克风供电）
WriteReg(0x64, 0x02);   // 充电截止电压 4.1V
WriteReg(0x61, 0x02);   // 预充 50mA
WriteReg(0x62, 0x08);   // 充电 200mA（0x0A=400mA）
WriteReg(0x63, 0x01);   // 终止 25mA
```

电池：PH2.0 3.7V 锂电座。

---

## 4. 对本项目的影响

1. **引脚全部确定，Phase 2 可直接开工**，无需等原理图。
2. **RGB 时序有实跑值**，不用自己调 —— 16MHz PCLK + 双缓冲 + bounce buffer 是出货固件验证过的组合。
3. **IMU 和 RTC 需要自己写驱动**，出货固件没碰。Phase 4 之前先做 I2C 扫描确认 QMI8658 / PCF85063 地址。
4. **PWR 短按可用性待验证**；若读不到就按方案降级到 BOOT 双击。
5. 出货固件的省电定时器是 `PowerSaveTimer(-1, 60, 300)`（60s 息屏 / 300s 关机）—— 本项目是常驻显示，**必须改掉或关掉**。
6. 分区表可复用其布局思路，但本项目不需要 `model`（唤醒词）和 6MB 的 ota_0，可以给应用和资源留更多空间。
