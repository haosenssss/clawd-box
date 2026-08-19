/*
 * Clawd Box 主循环。
 *
 * 板子是"厚"的一侧：会话表、计时、状态仲裁、页面轮播都在这里，
 * Mac 只负责把它读不到的东西（注册表、PID 存活、钩子事件）转发过来。
 */

#include <stdio.h>
#include <string.h>

#include "bsp/board_bsp.h"
#include "bsp/board_config.h"
#include "link/link.h"
#include "model/sessions.h"
#include "sprite/clawd.h"
#include "sprite/text.h"
#include "bsp/bsp_imu.h"
#include "input/input.h"
#include "motion.h"
#include "ui/pager.h"
#include "ui/pages.h"
#include "ui/theme.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static model_t s_model;

/*
 * 精灵区离屏合成缓冲。
 *
 * bounce buffer 模式下写帧缓冲是**立即可见**的，所以不能在帧缓冲里
 * "先擦后画"——那个中间态会被看到，就是闪动。
 * 改成：在 scratch 里擦+画，再整块拷过去。拷贝是一次线性写入，
 * 屏幕上看不到黑底中间态。
 */
static uint16_t *s_scratch = NULL;
static uint32_t s_frame_ms = 0;
static bsp_accel_t s_accel_dbg = {0};
static int s_scratch_w = 0;
static int s_scratch_h = 0;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 官方 189 个动词里挑的短款，便于在 480 宽内和项目名同行显示 */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    printf("\n\n######## Clawd Box ########\n");

    model_init(&s_model);
    s_model.limits.five_hour.pct = -1.0f;
    s_model.limits.seven_day.pct = -1.0f;

    ESP_ERROR_CHECK(bsp_board_init());
    ESP_ERROR_CHECK(bsp_display_init());

    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(bsp_display_get_framebuffer(&fb0, &fb1));
    uint16_t *const fb_main = (uint16_t *)fb0;
    (void)fb1;   /* num_fbs=1，只有一个缓冲 */
    ui_clear_all(fb_main, COL_BG);

    /* scratch 按精灵包围盒的最大可能尺寸一次分配，稳态零 malloc */
    /*
     * 离屏缓冲要**把重力位移的范围一起包进来**。
     * 合成区一旦跟着精灵移动，上一帧的位置就没人擦，会拖出残影；
     * 把区域固定成"静止包围盒 + 两侧各留 MOTION_MAX_PX"，
     * 精灵在里面动，区域本身不动，一次线性拷贝就把残影覆盖掉了。
     */
    s_scratch_w = (int)((CLAWD_UNIT_W + 4) * SPRITE_SCALE) + 8 + 2 * MOTION_MAX_PX_I;
    s_scratch_h = (int)((10 + 4) * SPRITE_SCALE) + 8 + 2 * MOTION_MAX_PX_I;
    s_scratch = heap_caps_malloc((size_t)s_scratch_w * s_scratch_h * sizeof(uint16_t),
                                 MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_scratch == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    printf("  离屏缓冲 %dx%d (%u KB)\n", s_scratch_w, s_scratch_h,
           (unsigned)((size_t)s_scratch_w * s_scratch_h * 2 / 1024));
    ESP_ERROR_CHECK(bsp_display_backlight(true));

    ESP_ERROR_CHECK(link_start(&s_model));
    printf("  就绪。等待主机推送…（PSRAM 余 %u KB）\n\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    ESP_ERROR_CHECK(input_init());
    const bool has_imu = (bsp_imu_init() == ESP_OK);
    motion_t motion;
    motion_init(&motion);

    pager_t pager;
    pager_init(&pager, now_ms());
    uint32_t motion_prev_ms = now_ms();

    uint32_t state_since = now_ms();
    clawd_state_t last_state = CLAWD_SLEEPING;
    int last_active = -1;
    page_kind_t last_kind = PAGE_ADMIN; /* 与初值不同，强制首帧整屏重绘 */
    uint32_t report_at = now_ms();
    uint32_t last_sig = 0;
    int chrome_dirty = 1;   /* 单帧缓冲，画一次即可 */

    while (true) {
        const uint32_t t = now_ms();

        /* 输入先于渲染：这一帧就要反映刚才那一下，不能拖到下一帧 */
        bool page_changed = pager_input(&pager, &s_model, input_poll(t), t);

        const int ring = model_ring_count(&s_model, t);
        session_t *focus = pager_tick(&pager, &s_model, t);
        if (pager.kind != last_kind) {
            last_kind = pager.kind;
            page_changed = true;
        }
        if (page_changed) {
            /* 换页要整屏擦——两页的版面不一样，残留会叠在一起 */
            ui_clear_all(fb_main, COL_BG);
            chrome_dirty = 2;
            last_sig = 0;
        }

        clawd_state_t state = focus != NULL ? model_clawd_state(focus, t) : CLAWD_SLEEPING;

        if (state == CLAWD_DONE && focus != NULL &&
            clawd_done_finished(t - focus->done_at_ms)) {
            focus->done_pending = false;
            state = model_clawd_state(focus, t);
        }
        if (state != last_state) {
            last_state = state;
            state_since = t;
        }

        const bool on_session_page = (pager.kind == PAGE_SESSION);
        uint16_t *fb = fb_main;
        const text_canvas_t tc = {
            .pixels = fb, .width = BSP_LCD_H_RES, .height = BSP_LCD_V_RES};
        const clawd_draw_t params = {
            .center_x = BSP_LCD_H_RES / 2,
            .baseline_y = SPRITE_BASELINE_Y,
            .px_per_unit = SPRITE_SCALE,
            .state = state,
            .elapsed_ms = t - state_since,
            .body_color = COL_BODY,
            .eye_color = COL_EYE,
            .shadow_color = COL_BG,
        };

        /* 重力：倾斜时精灵朝低处偏一点，晃动时弹一下。不是水平仪，克制为上。 */
        float dx = 0.0f, dy = 0.0f;
        if (on_session_page) {
            bsp_accel_t a = {0};
            const bool got = has_imu && bsp_imu_read(&a);
            const float dt = (float)(t - motion_prev_ms) / 1000.0f;
            motion_prev_ms = t;
            if (got) s_accel_dbg = a;
            if (motion_step(&motion, got, a.x, a.y, dt) && focus != NULL) {
                state_since = t; /* 晃一下＝把当前动作从头演一遍 */
            }
            dx = motion.x;
            dy = motion.y;
        }

      if (on_session_page) {
        clawd_rect_t box = clawd_bounds(&params);
        /* 静止包围盒向外扩出位移范围，让合成区固定不动 */
        box.x -= MOTION_MAX_PX_I;
        box.y -= MOTION_MAX_PX_I;
        box.w += 2 * MOTION_MAX_PX_I;
        box.h += 2 * MOTION_MAX_PX_I;
        if (box.w > s_scratch_w) box.w = s_scratch_w;
        if (box.h > s_scratch_h) box.h = s_scratch_h;

        /* 1) 在 scratch 里擦干净并画好整块内容 */
        for (int i = 0; i < box.w * box.h; i++) s_scratch[i] = COL_BG;
        const clawd_canvas_t scratch_canvas = {
            .pixels = s_scratch, .width = box.w, .height = box.h};
        const text_canvas_t scratch_text = {
            .pixels = s_scratch, .width = box.w, .height = box.h};
        clawd_draw_t local = params;
        local.center_x = params.center_x + (int)dx - box.x;
        local.baseline_y = params.baseline_y + (int)dy - box.y;
        clawd_draw(&scratch_canvas, &local);
        if (state == CLAWD_SLEEPING) {
            draw_zzz(&scratch_text, local.center_x + 104,
                     local.baseline_y - (int)(9.0f * SPRITE_SCALE) + 14, t);
        }

        /* 2) 整块拷进帧缓冲——一次线性写入，不暴露中间态 */
        for (int row = 0; row < box.h; row++) {
            const int dst_y = box.y + row;
            if (dst_y < 0 || dst_y >= BSP_LCD_V_RES) continue;
            memcpy(fb + (size_t)dst_y * BSP_LCD_H_RES + box.x,
                   s_scratch + (size_t)row * box.w, (size_t)box.w * sizeof(uint16_t));
        }
      }

        /* 静态部分只在内容变化时重画；两个缓冲各画一次 */
        const uint32_t now_unix_sig =
            s_model.host_unix_sec > 0
                ? s_model.host_unix_sec + (int64_t)((t - s_model.host_sync_ms) / 1000u)
                : 0;
        const uint32_t sig = on_session_page
                                 ? session_page_signature(&s_model, focus, state, t)
                                 : admin_page_signature(&s_model, now_unix_sig);
        if (sig != last_sig) {
            last_sig = sig;
            chrome_dirty = 2;
        }
        if (chrome_dirty > 0) {
            const int64_t now_unix =
                s_model.host_unix_sec > 0
                    ? s_model.host_unix_sec + (int64_t)((t - s_model.host_sync_ms) / 1000u)
                    : 0;
            if (on_session_page) {
                session_page_draw(fb, &tc, &s_model, focus, state, t, now_unix);
            } else {
                admin_page_draw(fb, &tc, &s_model, t, now_unix);
            }
            chrome_dirty--;
        }

        /* 单帧缓冲 + bounce：写入即显示，不需要 flush/交换 */

        if (ring != last_active || (uint32_t)(t - report_at) > 10000) {
            last_active = ring;
            report_at = t;
            printf("  页=%s 在环=%d/%d 焦点=%s 状态=%d sub=%d/%d ctx=%.0f%% 5h=%.0f%% wk=%.0f%% 帧=%lums 丢帧=%lu g=(%.2f,%.2f,%.2f)\n",
                   on_session_page ? "会话" : "管理",
                   ring, model_active_count(&s_model), focus ? focus->name : "-", (int)state,
                   focus ? model_sub_done(focus) : 0, focus ? model_sub_total(focus) : 0,
                   (double)(focus ? focus->ctx_pct : -1.0f),
                   (double)s_model.limits.five_hour.pct,
                   (double)s_model.limits.seven_day.pct,
                   (unsigned long)s_frame_ms, (unsigned long)link_dropped(),
                   (double)s_accel_dbg.x, (double)s_accel_dbg.y, (double)s_accel_dbg.z);
        }

        /*
         * **必须至少让出一个 tick。**
         * 原来超预算时调 taskYIELD()，但它只在同优先级之间轮转，
         * 不会调度到优先级更低的 IDLE——精灵放大后单帧超出预算，
         * 于是渲染任务再也不阻塞，IDLE0 饿死，任务看门狗每 5 秒报一次。
         */
        const int64_t spent = (int64_t)now_ms() - (int64_t)t;
        const TickType_t rest = spent < FRAME_MS ? pdMS_TO_TICKS(FRAME_MS - spent) : 0;
        vTaskDelay(rest > 0 ? rest : 1);
        s_frame_ms = (uint32_t)spent;
    }
}
