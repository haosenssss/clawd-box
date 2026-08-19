#include "bsp_imu.h"

#include "board_bsp.h"
#include "board_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "imu";

#define I2C_TIMEOUT_MS 50

#define QMI_REG_WHO_AM_I 0x00
#define QMI_REG_CTRL1 0x02
#define QMI_REG_CTRL2 0x03
#define QMI_REG_CTRL7 0x08
#define QMI_REG_AX_L 0x35

#define QMI_WHO_AM_I_VALUE 0x05
/* CTRL1: 地址自增（连读 6 个字节必须开），串口置为 I2C */
#define QMI_CTRL1_ADDR_AI 0x40
/* CTRL2: 量程 ±2g + ODR 62.5Hz。倾斜检测不需要更快，也不需要更大量程。 */
#define QMI_CTRL2_ACC_2G_62HZ 0x07
/* CTRL7: 只开加速度 */
#define QMI_CTRL7_ACC_EN 0x01

/*
 * 每 g 对应多少 LSB —— **必须回读 CTRL2 才知道**，不能按自己写进去的值算。
 * 实测写了 ±2g，读数却只有真实值的 1/4（静止平放 Z 轴 -0.25 而不是 -1.00），
 * 说明芯片实际工作在 ±8g。量程配置有没有生效，只有回读才能确定；
 * 按"我写了什么"去换算，误差是静默的，看起来只是"重力效果偏弱"。
 */
static float s_lsb_per_g = 16384.0f;



static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t qmi_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t qmi_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, I2C_TIMEOUT_MS);
}

/**
 * 静止标定：**合加速度必然是 1g**，用它反推 LSB/g。
 *
 * 不看量程位，因为量程位骗人：CTRL2 回读 0x07（按手册是 ±2g，16384 LSB/g），
 * 而静止平放读到 AZ = 0xF015 = -4075 —— 按 ±2g 换算是 -0.25g，
 * 重力不可能只有 0.25g；按 ±8g 换算恰好是 -0.995g。**芯片实际就在 ±8g。**
 *
 * 这类错误是静默的：数值一直有、方向也对，只是小了 4 倍，
 * 表现为"重力效果偏弱"，很容易去调弹簧参数而不是查量程。
 * 用物理常量标定就绕开了整个问题——量程位怎么解读都不影响结果。
 */
static esp_err_t calibrate_scale(void)
{
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t raw[6] = {0};
        if (qmi_read(QMI_REG_AX_L, raw, sizeof(raw)) == ESP_OK) {
            const int16_t ax = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
            const int16_t ay = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
            const int16_t az = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
            sum += sqrt((double)ax * ax + (double)ay * ay + (double)az * az);
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (n < 8) return ESP_ERR_INVALID_STATE;

    const double mag = sum / n;
    /* 上电时被拿在手里晃就标不准，宁可用手册值也不要用一个荒谬的比例 */
    if (mag < 1500.0 || mag > 30000.0) return ESP_ERR_INVALID_STATE;
    s_lsb_per_g = (float)mag;
    return ESP_OK;
}

esp_err_t bsp_imu_init(void)
{
    if (s_dev != NULL) return ESP_OK;

    /* 地址实测是 0x6B，不是常见默认的 0x6A */
    if (i2c_master_probe(bsp_i2c_bus(), BSP_ADDR_QMI8658, 100) != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X 没应答", BSP_ADDR_QMI8658);
        return ESP_ERR_NOT_FOUND;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_ADDR_QMI8658,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bsp_i2c_bus(), &cfg, &s_dev);
    if (err != ESP_OK) return err;

    uint8_t who = 0;
    if (qmi_read(QMI_REG_WHO_AM_I, &who, 1) != ESP_OK || who != QMI_WHO_AM_I_VALUE) {
        ESP_LOGW(TAG, "WHO_AM_I=0x%02X（期望 0x%02X）", who, QMI_WHO_AM_I_VALUE);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* 顺序有讲究：先开地址自增，再配量程，最后使能 */
    ESP_RETURN_ON_ERROR(qmi_write(QMI_REG_CTRL1, QMI_CTRL1_ADDR_AI), TAG, "");
    ESP_RETURN_ON_ERROR(qmi_write(QMI_REG_CTRL2, QMI_CTRL2_ACC_2G_62HZ), TAG, "");
    ESP_RETURN_ON_ERROR(qmi_write(QMI_REG_CTRL7, QMI_CTRL7_ACC_EN), TAG, "");

    /* 等第一批采样出来再标定 */
    vTaskDelay(pdMS_TO_TICKS(50));
    const bool ok = (calibrate_scale() == ESP_OK);
    ESP_LOGI(TAG, "QMI8658 就绪 @0x%02X，%.0f LSB/g（%s）", BSP_ADDR_QMI8658,
             (double)s_lsb_per_g, ok ? "静止标定" : "标定失败，用手册值");
    return ESP_OK;
}

bool bsp_imu_read(bsp_accel_t *out)
{
    if (s_dev == NULL) return false;
    uint8_t raw[6] = {0};
    if (qmi_read(QMI_REG_AX_L, raw, sizeof(raw)) != ESP_OK) return false;

    const int16_t ax = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    const int16_t ay = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    const int16_t az = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
    out->x = (float)ax / s_lsb_per_g;
    out->y = (float)ay / s_lsb_per_g;
    out->z = (float)az / s_lsb_per_g;
    return true;
}
