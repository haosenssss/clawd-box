/* QMI8658 加速度计。只用加速度——陀螺仪对"重力让精灵晃一下"毫无帮助，关掉省电。 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    /** 单位 g。静止平放时应有一个轴接近 ±1.0 */
    float x, y, z;
} bsp_accel_t;

esp_err_t bsp_imu_init(void);
/** 读一次。未初始化或 I2C 出错返回 false（调用方保留上一次的值即可）。 */
bool bsp_imu_read(bsp_accel_t *out);
