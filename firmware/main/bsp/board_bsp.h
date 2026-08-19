/*
 * 板级支持包对外接口。
 *
 * 上电顺序是有强制依赖的，必须按 bsp_board_init() 内的次序：
 *   I2C → AXP2101（把电轨打起来）→ TCA9554 → LCD 复位脉冲 → ST7701
 * 跳过或颠倒会导致屏幕不亮，且现象像"驱动写错了"，很难查。
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_ops.h"
#include <stdbool.h>

/* ---------------- 总线与电源 ---------------- */

/** I2C + AXP2101 + TCA9554（含 LCD 复位脉冲）。显示初始化前必须先调。 */
esp_err_t bsp_board_init(void);

i2c_master_bus_handle_t bsp_i2c_bus(void);
esp_io_expander_handle_t bsp_io_expander(void);

/** 读 AXP2101 电池电压（毫伏）。失败返回 0。 */
uint16_t bsp_battery_millivolts(void);

/* ---------------- 显示 ---------------- */

/** 初始化 ST7701 + RGB 面板。调用前必须先 bsp_board_init()。 */
esp_err_t bsp_display_init(void);

esp_lcd_panel_handle_t bsp_display_panel(void);

/** 背光开关。注意本板背光逻辑反相，这里已处理，传 true 就是亮。 */
esp_err_t bsp_display_backlight(bool on);

/**
 * 取当前正在使用的帧缓冲指针（RGB565）。
 * num_fbs=2 时 esp_lcd 会做前后台交换，画完要调 bsp_display_flush()。
 */
esp_err_t bsp_display_get_framebuffer(void **fb0, void **fb1);

/** 把整帧推给面板（触发缓冲交换，交换在下一个 VSYNC 生效）。 */
esp_err_t bsp_display_flush(const void *bitmap);

/**
 * 等缓冲交换真正完成。
 *
 * **不等就会频闪**：draw_bitmap 只是打个标记就返回，真正的切换发生在
 * VSYNC 中断里。若立刻去画另一个缓冲，画的可能正是还在扫描输出的那一块，
 * 于是屏幕上能看到"擦除→重画"的中间态。
 */
void bsp_display_wait_swap(void);
