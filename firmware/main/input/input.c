#include "input.h"

#include "bsp_touch.h"
#include "board_bsp.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "input";

/* ---- 手势阈值 ---- */
/*
 * 滑动阈值。**实测调过**：最初 60px / 800ms 太苛刻，日志里大量真实滑动被丢弃：
 *     dx=59  dt=1140ms  ← 划得慢了一点就超时
 *     dx=36  dt=90ms    ← 快速轻扫不到 60px
 * 4 寸屏上手指行程本来就短，而且贴墙操作往往是慢慢划的。
 * 放宽到 36px / 1600ms，误触由"点按"那条更严的判据兜住。
 */
#define SWIPE_MIN_PX 36
#define SWIPE_MAX_MS 1600
/* 点按：位移和时长都要小 */
#define TAP_MAX_PX 24
#define TAP_MAX_MS 300

/* ---- BOOT 键 ---- */
#define BTN_DEBOUNCE_MS 30
#define BTN_LONG_MS 1000
/* 双击间隔。单击要等这么久才能确定"没有第二下"，所以别设太长。 */
#define BTN_DOUBLE_MS 350

/* ---- AXP2101 PWR 键 ----
 * 只读中断状态位并写 1 清除；不碰电轨、不碰关机寄存器。
 * 长按关机是 PMU 硬件行为，与这里无关。 */
#define AXP_REG_IRQ_EN2 0x41
#define AXP_REG_IRQ_STATUS2 0x49
#define AXP_IRQ2_PKEY_SHORT 0x08

typedef struct {
    bool down;
    int16_t x0;
    uint32_t t0;
} touch_state_t;

static touch_state_t s_touch;
static bool s_touch_ok = false;

static bool s_btn_down = false;
static uint32_t s_btn_t0 = 0;
static bool s_btn_long_fired = false;
static uint32_t s_btn_click_at = 0; /* 上一次单击的时刻，0 = 没有待定的单击 */

static bool s_pwr_ok = false;
static int16_t s_tap_y = -1;

int16_t input_tap_y(void) { return s_tap_y; }

esp_err_t input_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BSP_BTN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    s_touch_ok = (bsp_touch_init() == ESP_OK);
    if (!s_touch_ok) ESP_LOGW(TAG, "触摸不可用，仅实体键");

    /* 打开 PWR 键短按中断。失败就当这块板子读不到，退回 BOOT 双击。 */
    uint8_t en = 0;
    if (bsp_pmic_read_reg(AXP_REG_IRQ_EN2, &en) == ESP_OK &&
        bsp_pmic_write_reg(AXP_REG_IRQ_EN2, en | AXP_IRQ2_PKEY_SHORT) == ESP_OK) {
        bsp_pmic_write_reg(AXP_REG_IRQ_STATUS2, AXP_IRQ2_PKEY_SHORT); /* 清历史 */
        s_pwr_ok = true;
    } else {
        ESP_LOGW(TAG, "PWR 键中断读不到，确认动作用 BOOT 双击");
    }
    return ESP_OK;
}

/** 触摸：按下记起点，抬起时判定。 */
static input_event_t poll_touch(uint32_t now_ms)
{
    if (!s_touch_ok) return INPUT_NONE;

    bsp_touch_point_t p = {0};
    /* 读失败**不能当成抬起**——否则一次 I2C 抖动就把滑动切成两半 */
    if (!bsp_touch_read(&p)) return INPUT_NONE;

    if (p.pressed) {
        if (!s_touch.down) {
            s_touch.down = true;
            s_touch.x0 = p.x;
            s_touch.t0 = now_ms;
        }
        return INPUT_NONE;
    }

    if (!s_touch.down) return INPUT_NONE;
    s_touch.down = false;

    const uint32_t dt = now_ms - s_touch.t0;
    /* 抬起这一帧读到的坐标已经无效，用最后一次按下的位置来判 */
    const int dx = (int)p.x - (int)s_touch.x0;

    ESP_LOGI(TAG, "抬起: dx=%d dt=%ums (起点 x=%d, y=%d)", dx, (unsigned)dt,
             (int)s_touch.x0, (int)p.y);
    if (dt < SWIPE_MAX_MS && dx >= SWIPE_MIN_PX) return INPUT_GO_LEFT;
    if (dt < SWIPE_MAX_MS && dx <= -SWIPE_MIN_PX) return INPUT_GO_RIGHT;
    if (dt < TAP_MAX_MS && dx > -TAP_MAX_PX && dx < TAP_MAX_PX) {
        s_tap_y = p.y; /* 管理页要用它定位点到了哪一行 */
        return INPUT_ACK;
    }
    return INPUT_NONE;
}

/** BOOT：短按=下一页，长按=回管理页，双击=确认。 */
static input_event_t poll_button(uint32_t now_ms)
{
    const bool pressed = gpio_get_level(BSP_BTN_BOOT) == 0; /* 低有效 */

    if (pressed && !s_btn_down) {
        s_btn_down = true;
        s_btn_t0 = now_ms;
        s_btn_long_fired = false;
        return INPUT_NONE;
    }

    if (pressed && s_btn_down) {
        if (!s_btn_long_fired && now_ms - s_btn_t0 >= BTN_LONG_MS) {
            s_btn_long_fired = true;
            s_btn_click_at = 0; /* 长按吃掉待定的单击 */
            return INPUT_GO_LEFT;
        }
        return INPUT_NONE;
    }

    if (!pressed && s_btn_down) {
        s_btn_down = false;
        const uint32_t dt = now_ms - s_btn_t0;
        if (s_btn_long_fired || dt < BTN_DEBOUNCE_MS) return INPUT_NONE;

        if (s_btn_click_at != 0 && now_ms - s_btn_click_at <= BTN_DOUBLE_MS) {
            s_btn_click_at = 0;
            return INPUT_ACK; /* 第二下 */
        }
        s_btn_click_at = now_ms; /* 第一下，先挂起等双击 */
        return INPUT_NONE;
    }

    /* 挂起的单击等够了双击窗口，确认它就是单击 */
    if (s_btn_click_at != 0 && now_ms - s_btn_click_at > BTN_DOUBLE_MS) {
        s_btn_click_at = 0;
        return INPUT_GO_RIGHT;
    }
    return INPUT_NONE;
}

static input_event_t poll_pwr(void)
{
    if (!s_pwr_ok) return INPUT_NONE;
    uint8_t st = 0;
    if (bsp_pmic_read_reg(AXP_REG_IRQ_STATUS2, &st) != ESP_OK) return INPUT_NONE;
    if ((st & AXP_IRQ2_PKEY_SHORT) == 0) return INPUT_NONE;
    bsp_pmic_write_reg(AXP_REG_IRQ_STATUS2, AXP_IRQ2_PKEY_SHORT); /* 写 1 清 */
    return INPUT_ACK;
}

static const char *EVENT_NAME[] = {"-", "去管理页", "下一页", "确认"};

input_event_t input_poll(uint32_t now_ms)
{
    const char *src = "触摸";
    input_event_t e = poll_touch(now_ms);
    if (e == INPUT_NONE) { e = poll_button(now_ms); src = "BOOT"; s_tap_y = -1; }
    if (e == INPUT_NONE) { e = poll_pwr(); src = "PWR"; s_tap_y = -1; }
    /* 输入事件很稀疏，全部打日志——手感对不对只能靠这行来对账 */
    if (e != INPUT_NONE) ESP_LOGI(TAG, "%s -> %s", src, EVENT_NAME[e]);
    return e;
}
