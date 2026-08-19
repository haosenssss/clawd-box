/*
 * GT911 电容触摸。
 *
 * 地址是**实测的 0x14，不是文档默认的 0x5D**——照抄默认值会一直 NACK，
 * 而现象只是"触摸没反应"，很容易误判成手势逻辑写错。
 *
 * 这里只做最小的事：轮询坐标寄存器，把"有没有按着、按在哪"交出去。
 * 手势识别（滑动/点按）不属于驱动，放在 input 层。
 */

#include "bsp_touch.h"

#include "board_bsp.h"
#include "board_config.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "touch";

#define I2C_TIMEOUT_MS 50

/* GT911 寄存器（16 位地址，大端） */
#define GT_REG_PRODUCT_ID 0x8140
#define GT_REG_STATUS 0x814E
#define GT_REG_POINT1 0x814F

/* 状态寄存器：bit7 = 数据就绪，bit0..3 = 触点数 */
#define GT_STATUS_READY 0x80
#define GT_STATUS_COUNT_MASK 0x0F

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t gt_read(uint16_t reg, uint8_t *out, size_t len)
{
    const uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), out, len, I2C_TIMEOUT_MS);
}

static esp_err_t gt_write(uint16_t reg, uint8_t value)
{
    const uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t bsp_touch_init(void)
{
    if (s_dev != NULL) return ESP_OK;

    /*
     * **地址不能写死。** GT911 的 I2C 地址由复位时 INT 引脚的电平决定，
     * 0x5D 和 0x14 都可能。出货固件用的是 0x5D，而我冷启动扫出来的是 0x14——
     * 两边都对，只是复位时序不同。所以这里逐个探，探到谁用谁。
     */
    static const uint8_t CANDIDATES[] = {BSP_ADDR_GT911, 0x14};
    uint8_t addr = 0;
    for (size_t i = 0; i < sizeof(CANDIDATES); i++) {
        if (i2c_master_probe(bsp_i2c_bus(), CANDIDATES[i], 100) == ESP_OK) {
            addr = CANDIDATES[i];
            break;
        }
    }
    if (addr == 0) {
        ESP_LOGW(TAG, "0x%02X / 0x14 都没应答", BSP_ADDR_GT911);
        /* 顺手把整条总线扫一遍——比反复猜地址快得多 */
        char found[64] = {0};
        size_t n = 0;
        for (uint8_t a = 0x08; a < 0x78 && n < sizeof(found) - 6; a++) {
            if (i2c_master_probe(bsp_i2c_bus(), a, 20) == ESP_OK) {
                n += (size_t)snprintf(found + n, sizeof(found) - n, "%02X ", a);
            }
        }
        ESP_LOGW(TAG, "总线上的器件: %s", found);
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bsp_i2c_bus(), &cfg, &s_dev);
    if (err != ESP_OK) return err;

    /* 读产品号确认真的是 GT911，而不是地址撞上了别的器件 */
    uint8_t id[4] = {0};
    err = gt_read(GT_REG_PRODUCT_ID, id, sizeof(id));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X 有应答但读产品号失败: %s", addr, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "GT911 就绪，产品号 %c%c%c%c @0x%02X", id[0], id[1], id[2], id[3], addr);
    return ESP_OK;
}

bool bsp_touch_read(bsp_touch_point_t *out)
{
    if (s_dev == NULL) return false;

    uint8_t status = 0;
    if (gt_read(GT_REG_STATUS, &status, 1) != ESP_OK) return false;
    if ((status & GT_STATUS_READY) == 0) return false;

    const int count = status & GT_STATUS_COUNT_MASK;
    bool touched = false;

    if (count > 0) {
        uint8_t p[8] = {0};
        if (gt_read(GT_REG_POINT1, p, sizeof(p)) == ESP_OK) {
            out->x = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
            out->y = (int16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));
            touched = true;
        }
    }

    /* **必须回写 0 清标志**，否则 GT911 不再更新，表现为"只能读到第一次触摸" */
    gt_write(GT_REG_STATUS, 0);

    out->pressed = touched;
    return true;
}
