/* GT911 触摸最小接口：只报"按没按、按在哪"，手势识别在 input 层。 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t x;
    int16_t y;
    bool pressed;
} bsp_touch_point_t;

/** 需要先 bsp_board_init()（I2C 总线要先起来）。 */
esp_err_t bsp_touch_init(void);

/**
 * 轮询一次。
 * 返回 false = 这次没读到新数据（芯片还没就绪或 I2C 出错），**不代表松手**——
 * 调用方应保留上一次的状态，不要把它当成抬起，否则滑动会被切断。
 */
bool bsp_touch_read(bsp_touch_point_t *out);
