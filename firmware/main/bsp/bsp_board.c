/*
 * I2C 总线 + AXP2101 电源 + TCA9554 IO 扩展。
 *
 * 上电顺序不可颠倒：先把电轨打起来，再配 IO 扩展器，最后给 LCD 一个复位脉冲。
 * AXP2101 的寄存器序列逐字取自出货固件——它决定了哪些电轨开、充电参数是多少。
 */

#include "board_bsp.h"
#include "board_config.h"

#include "esp_check.h"
#include "esp_io_expander_tca9554.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

#define I2C_TIMEOUT_MS 100
#define I2C_SPEED_HZ 400000

/* AXP2101 寄存器 */
#define AXP_REG_ADC_ENABLE 0x30
#define AXP_REG_VBAT_H 0x34
#define AXP_REG_VBAT_L 0x35
#define AXP_ADC_EN_VBAT 0x01

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_pmic = NULL;
static esp_io_expander_handle_t s_expander = NULL;

i2c_master_bus_handle_t bsp_i2c_bus(void) { return s_bus; }
esp_io_expander_handle_t bsp_io_expander(void) { return s_expander; }

/* ------------------------------------------------------------------ */

static esp_err_t i2c_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

static esp_err_t pmic_write(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_pmic, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t pmic_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_pmic, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

esp_err_t bsp_pmic_read_reg(uint8_t reg, uint8_t *value)
{
    if (s_pmic == NULL) return ESP_ERR_INVALID_STATE;
    return pmic_read(reg, value);
}

esp_err_t bsp_pmic_write_reg(uint8_t reg, uint8_t value)
{
    if (s_pmic == NULL) return ESP_ERR_INVALID_STATE;
    return pmic_write(reg, value);
}

/*
 * AXP2101 配置。序列与出货固件一致——这不是随意的默认值，
 * 而是这块板子实际的电轨拓扑（只用 DC1 供主轨，ALDO1 供麦克风）。
 */
static esp_err_t pmic_init(void)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_ADDR_AXP2101,
        .scl_speed_hz = I2C_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_pmic), TAG,
                        "AXP2101 挂载失败");

    /* {寄存器, 值} —— 注释见 docs/pinout.md §3.6 */
    static const uint8_t SEQ[][2] = {
        {0x22, 0x06}, /* PWRON > OFFLEVEL 作为关机源 */
        {0x27, 0x10}, /* 长按 4s 关机 */
        {0x80, 0x01}, /* 只留 DC1 */
        {0x90, 0x00}, /* 先关全部 LDO */
        {0x91, 0x00},
        {0x82, (3300 - 1500) / 100}, /* DC1 = 3.3V 主轨 */
        {0x92, (3300 - 500) / 100},  /* ALDO1 = 3.3V */
        {0x90, 0x01},                /* 使能 ALDO1（麦克风） */
        {0x64, 0x02},                /* 充电截止 4.1V */
        {0x61, 0x02},                /* 预充 50mA */
        {0x62, 0x08},                /* 恒流 200mA */
        {0x63, 0x01},                /* 终止 25mA */
        {AXP_REG_ADC_ENABLE, AXP_ADC_EN_VBAT}, /* 打开电池电压 ADC */
    };

    for (size_t i = 0; i < sizeof(SEQ) / sizeof(SEQ[0]); i++) {
        esp_err_t err = pmic_write(SEQ[i][0], SEQ[i][1]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AXP2101 写 0x%02X 失败: %s", SEQ[i][0], esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "AXP2101 就绪");
    return ESP_OK;
}

uint16_t bsp_battery_millivolts(void)
{
    if (s_pmic == NULL) return 0;
    uint8_t hi = 0, lo = 0;
    if (pmic_read(AXP_REG_VBAT_H, &hi) != ESP_OK) return 0;
    if (pmic_read(AXP_REG_VBAT_L, &lo) != ESP_OK) return 0;
    /* 14 位，单位直接就是毫伏 */
    return (uint16_t)(((hi & 0x3F) << 8) | lo);
}

/*
 * TCA9554 初始化 + LCD 复位脉冲。
 * 三个 200ms 延时是出货固件里就有的，不要缩短——面板对复位时序敏感。
 */
static esp_err_t expander_init(void)
{
    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_tca9554(s_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
                                        &s_expander),
        TAG, "TCA9554 初始化失败");

    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(s_expander,
                                BSP_EXP_ENABLE | BSP_EXP_LCD_RST | IO_EXPANDER_PIN_NUM_6,
                                IO_EXPANDER_OUTPUT),
        TAG, "设置方向失败");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, BSP_EXP_ENABLE, 1), TAG, "");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, IO_EXPANDER_PIN_NUM_6, 0), TAG, "");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, BSP_EXP_LCD_RST, 0), TAG, "");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, BSP_EXP_LCD_RST, 1), TAG, "");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(s_expander,
                                IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_6,
                                IO_EXPANDER_INPUT),
        TAG, "");

    ESP_LOGI(TAG, "TCA9554 就绪，LCD 已复位");
    return ESP_OK;
}

esp_err_t bsp_board_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "I2C 初始化失败");
    ESP_RETURN_ON_ERROR(pmic_init(), TAG, "电源初始化失败");
    ESP_RETURN_ON_ERROR(expander_init(), TAG, "IO 扩展初始化失败");
    return ESP_OK;
}
