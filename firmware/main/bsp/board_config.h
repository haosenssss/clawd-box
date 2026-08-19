/*
 * Waveshare ESP32-S3-Touch-LCD-4B 板级常量。
 *
 * 全部来自 docs/pinout.md —— 引脚取自出货固件的开源板级定义，
 * I2C 地址为本机上机扫描实测。**没有一处是猜的。**
 */
#pragma once

#include "driver/gpio.h"

/* ---------------- I2C：一条总线挂全部器件 ---------------- */
#define BSP_I2C_PORT I2C_NUM_0
#define BSP_I2C_SDA GPIO_NUM_47
#define BSP_I2C_SCL GPIO_NUM_48

/* 实测地址。注意 GT911 与 QMI8658 用的都是**备选地址**，不是常见默认值。 */
#define BSP_ADDR_ES8311 0x18
#define BSP_ADDR_TCA9554 0x20
#define BSP_ADDR_AXP2101 0x34
#define BSP_ADDR_ES7210 0x40
#define BSP_ADDR_PCF85063 0x51 /* RTC，出货固件未使用 */
#define BSP_ADDR_GT911 0x14    /* ⚠ 不是默认的 0x5D */
#define BSP_ADDR_QMI8658 0x6B  /* ⚠ 不是默认的 0x6A，出货固件未使用 */

/* ---------------- 显示：ST7701 480x480 RGB 并口 ---------------- */
#define BSP_LCD_H_RES 480
#define BSP_LCD_V_RES 480

#define BSP_LCD_VSYNC GPIO_NUM_3
#define BSP_LCD_HSYNC GPIO_NUM_46
#define BSP_LCD_DE GPIO_NUM_17
#define BSP_LCD_PCLK GPIO_NUM_9

#define BSP_LCD_DATA0 GPIO_NUM_40
#define BSP_LCD_DATA1 GPIO_NUM_41
#define BSP_LCD_DATA2 GPIO_NUM_42
#define BSP_LCD_DATA3 GPIO_NUM_2
#define BSP_LCD_DATA4 GPIO_NUM_1
#define BSP_LCD_DATA5 GPIO_NUM_21
#define BSP_LCD_DATA6 GPIO_NUM_8
#define BSP_LCD_DATA7 GPIO_NUM_18
#define BSP_LCD_DATA8 GPIO_NUM_45
#define BSP_LCD_DATA9 GPIO_NUM_38
#define BSP_LCD_DATA10 GPIO_NUM_39
#define BSP_LCD_DATA11 GPIO_NUM_10
#define BSP_LCD_DATA12 GPIO_NUM_11
#define BSP_LCD_DATA13 GPIO_NUM_12
#define BSP_LCD_DATA14 GPIO_NUM_13
#define BSP_LCD_DATA15 GPIO_NUM_14

/* 背光输出逻辑反相：拉低才是亮 */
#define BSP_LCD_BACKLIGHT GPIO_NUM_4
#define BSP_LCD_BACKLIGHT_ON_LEVEL 0

/* RGB 时序——出货固件实跑值，不要自行调整 */
#define BSP_LCD_PCLK_HZ (16 * 1000 * 1000)
#define BSP_LCD_HSYNC_PULSE 10
#define BSP_LCD_HSYNC_BACK 10
#define BSP_LCD_HSYNC_FRONT 20
#define BSP_LCD_VSYNC_PULSE 10
#define BSP_LCD_VSYNC_BACK 10
#define BSP_LCD_VSYNC_FRONT 10

/* 单帧缓冲 + bounce buffer。
 * bounce buffer **必须开**：去掉它之后 RGB 外设直接从 PSRAM 取像素，
 * 与 CPU 的渲染写入抢带宽，DMA 欠载会让整幅画面持续横向漂移。
 * 闪动不靠双缓冲解决，改用离屏合成（见 main.c 的 scratch 缓冲）。 */
#define BSP_LCD_NUM_FB 1
#define BSP_LCD_BOUNCE_PX (BSP_LCD_H_RES * 20)

/* ---------------- TCA9554 位分配 ---------------- */
#define BSP_EXP_LCD_CS IO_EXPANDER_PIN_NUM_0
#define BSP_EXP_LCD_SDA IO_EXPANDER_PIN_NUM_1
#define BSP_EXP_LCD_SCL IO_EXPANDER_PIN_NUM_2
#define BSP_EXP_ENABLE IO_EXPANDER_PIN_NUM_3 /* 上电置 1 */
#define BSP_EXP_LCD_RST IO_EXPANDER_PIN_NUM_5 /* 低脉冲复位 */

/* ---------------- 音频 ---------------- */
#define BSP_I2S_MCLK GPIO_NUM_5
#define BSP_I2S_WS GPIO_NUM_7
#define BSP_I2S_BCLK GPIO_NUM_16
#define BSP_I2S_DIN GPIO_NUM_15
#define BSP_I2S_DOUT GPIO_NUM_6
#define BSP_AUDIO_SAMPLE_RATE 24000

/* ---------------- 按键 ---------------- */
#define BSP_BTN_BOOT GPIO_NUM_0
