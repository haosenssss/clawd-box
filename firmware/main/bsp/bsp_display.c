/*
 * ST7701 + 480x480 RGB 并口面板。
 *
 * 这块屏有个容易踩的结构：**寄存器初始化走 3-wire SPI，而那三根脚在 TCA9554 上**，
 * 像素数据才走 16 根 RGB 并口。所以必须先有 IO 扩展器，再建 panel_io。
 *
 * 帧缓冲放 PSRAM（双缓冲 900KB），配 bounce buffer 避免 PSRAM 带宽不足导致的撕裂。
 */

#include "board_bsp.h"
#include "board_config.h"
#include "st7701_init_cmds.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static SemaphoreHandle_t s_swap_done = NULL;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               const esp_lcd_rgb_panel_event_data_t *edata, void *arg)
{
    (void)panel; (void)edata; (void)arg;
    BaseType_t woken = pdFALSE;
    if (s_swap_done != NULL) xSemaphoreGiveFromISR(s_swap_done, &woken);
    return woken == pdTRUE;
}

esp_lcd_panel_handle_t bsp_display_panel(void) { return s_panel; }

esp_err_t bsp_display_backlight(bool on)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "背光引脚配置失败");
    /* 本板背光反相：on 对应输出低电平 */
    const int level = on ? BSP_LCD_BACKLIGHT_ON_LEVEL : !BSP_LCD_BACKLIGHT_ON_LEVEL;
    return gpio_set_level(BSP_LCD_BACKLIGHT, level);
}

esp_err_t bsp_display_init(void)
{
    ESP_RETURN_ON_FALSE(bsp_io_expander() != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "必须先 bsp_board_init()");

    /* 1) 3-wire SPI —— 三根脚都在 IO 扩展器上，只用来发初始化寄存器 */
    const spi_line_config_t line_cfg = {
        .cs_io_type = IO_TYPE_EXPANDER,
        .cs_expander_pin = BSP_EXP_LCD_CS,
        .scl_io_type = IO_TYPE_EXPANDER,
        .scl_expander_pin = BSP_EXP_LCD_SCL,
        .sda_io_type = IO_TYPE_EXPANDER,
        .sda_expander_pin = BSP_EXP_LCD_SDA,
        .io_expander = bsp_io_expander(),
    };
    esp_lcd_panel_io_3wire_spi_config_t io_cfg = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_cfg, 0);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_cfg, &s_panel_io), TAG,
                        "3-wire SPI 创建失败");

    /* 2) RGB 并口。时序为出货固件实跑值。 */
    esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .psram_trans_align = 64,
        .data_width = 16,
        .bits_per_pixel = 16,
        .de_gpio_num = BSP_LCD_DE,
        .pclk_gpio_num = BSP_LCD_PCLK,
        .vsync_gpio_num = BSP_LCD_VSYNC,
        .hsync_gpio_num = BSP_LCD_HSYNC,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums =
            {
                BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
                BSP_LCD_DATA4, BSP_LCD_DATA5, BSP_LCD_DATA6, BSP_LCD_DATA7,
                BSP_LCD_DATA8, BSP_LCD_DATA9, BSP_LCD_DATA10, BSP_LCD_DATA11,
                BSP_LCD_DATA12, BSP_LCD_DATA13, BSP_LCD_DATA14, BSP_LCD_DATA15,
            },
        .timings =
            {
                .pclk_hz = BSP_LCD_PCLK_HZ,
                .h_res = BSP_LCD_H_RES,
                .v_res = BSP_LCD_V_RES,
                .hsync_pulse_width = BSP_LCD_HSYNC_PULSE,
                .hsync_back_porch = BSP_LCD_HSYNC_BACK,
                .hsync_front_porch = BSP_LCD_HSYNC_FRONT,
                .vsync_pulse_width = BSP_LCD_VSYNC_PULSE,
                .vsync_back_porch = BSP_LCD_VSYNC_BACK,
                .vsync_front_porch = BSP_LCD_VSYNC_FRONT,
                .flags.pclk_active_neg = false,
            },
        .num_fbs = BSP_LCD_NUM_FB,
        .bounce_buffer_size_px = BSP_LCD_BOUNCE_PX,
        .flags.fb_in_psram = true,
    };

    st7701_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .rgb_config = &rgb_cfg,
        .flags =
            {
                .mirror_by_cmd = 0,
                .auto_del_panel_io = 1,
            },
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, /* 复位已由 TCA9554 完成 */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(s_panel_io, &panel_cfg, &s_panel), TAG,
                        "ST7701 创建失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "复位失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "初始化失败");

    s_swap_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_swap_done != NULL, ESP_ERR_NO_MEM, TAG, "信号量创建失败");
    const esp_lcd_rgb_panel_event_callbacks_t cbs = {.on_vsync = on_vsync};
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL),
                        TAG, "VSYNC 回调注册失败");

    ESP_LOGI(TAG, "ST7701 %dx%d 就绪，PCLK %d MHz，%d 缓冲区",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_PCLK_HZ / 1000000, BSP_LCD_NUM_FB);
    return ESP_OK;
}

esp_err_t bsp_display_get_framebuffer(void **fb0, void **fb1)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "面板未初始化");
    return esp_lcd_rgb_panel_get_frame_buffer(s_panel, BSP_LCD_NUM_FB, fb0, fb1);
}

esp_err_t bsp_display_flush(const void *bitmap)
{
    ESP_RETURN_ON_FALSE(s_panel != NULL, ESP_ERR_INVALID_STATE, TAG, "面板未初始化");
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, bitmap);
}

void bsp_display_wait_swap(void)
{
    if (s_swap_done == NULL) return;
    /* 超时兜底：万一 VSYNC 停了也不能把渲染线程卡死 */
    xSemaphoreTake(s_swap_done, pdMS_TO_TICKS(100));
}
