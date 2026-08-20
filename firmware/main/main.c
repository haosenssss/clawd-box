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
#include "audio/audio.h"
#include "input/input.h"
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
/*
 * 界面静态部分的整屏离屏缓冲。450KB 放 PSRAM——比起"滑动时看到一下整屏黑闪"，
 * 这点内存完全值得（总共 8MB，帧缓冲自己就占 450KB）。
 */
static uint16_t *s_page = NULL;
static uint16_t *s_scratch = NULL;
static uint32_t s_frame_ms = 0;
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
    /* +17 = clawd_bounds 里 8.5 的两倍余量。DJ 态的灯要铺满屏宽，
     * 缓冲不跟着放大的话，box.w 会被下面的 clamp 砍回去，灯就又被切了。 */
    s_scratch_w = (int)((CLAWD_UNIT_W + 17) * SPRITE_SCALE) + 8;
    s_scratch_h = (int)((10 + 17) * SPRITE_SCALE) + 8;
    s_page = heap_caps_malloc((size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
                              MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_page == NULL ? ESP_ERR_NO_MEM : ESP_OK);
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
    /* 没喇叭不致命——界面照常工作，只是不出声 */
    if (audio_init() != ESP_OK) printf("  音频不可用，静音运行\n");
    pager_t pager;
    pager_init(&pager, now_ms());

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
            chrome_dirty = 2;
            last_sig = 0;
        }

        clawd_state_t state = focus != NULL ? model_clawd_state(focus, t) : CLAWD_SLEEPING;

        /*
         * 提示音的触发点。**视觉和听觉在这里分开**：
         * 动作一直在演（就在下面），这里只决定什么时候再响一次。
         *
         * 完成音走边沿——真实的"干完了"跃迁才响，且只响一次，那是通知不是催促；
         * 等待输入走退避重复，直到你处理或按键确认。
         */
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session_t *s = model_at(&s_model, i);
            if (s == NULL) break;
            if (model_reminder_due(s, t)) audio_play(SOUND_NEEDS_YOU);
            /* **任何**会话干完都该出声，不只是当前聚焦的那个——
             * 你多开终端的意义就在于不用盯着看，声音是唯一的跨页通知。 */
            if (s->done_pending && !s->done_chimed) {
                s->done_chimed = true;
                audio_play(SOUND_DONE);
            }
        }
        /* 限额告急只在跨过 95% 的那一次响 */
        {
            const bool critical = s_model.limits.five_hour.pct >= 95.0f ||
                                  s_model.limits.seven_day.pct >= 95.0f;
            static bool was_critical = false;
            if (critical && !was_critical) audio_play(SOUND_LIMIT);
            was_critical = critical;
        }

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

        /*
         * 界面静态部分：**整屏离屏合成，再一次线性拷过去**。
         *
         * 单帧缓冲 + bounce 模式下写帧缓冲是立即可见的，所以"先擦后画"的
         * 中间态会被眼睛抓到——滑动换页时那一下黑闪就是整屏擦被看见了。
         * 在离屏缓冲里擦好画好再整块拷贝，屏幕上就只有前后两个完整画面。
         */
        const int64_t now_unix =
            s_model.host_unix_sec > 0
                ? s_model.host_unix_sec + (int64_t)((t - s_model.host_sync_ms) / 1000u)
                : 0;
        const uint32_t sig = on_session_page
                                 ? session_page_signature(&s_model, focus, state, t)
                                 : admin_page_signature(&s_model, now_unix);
        if (sig != last_sig) {
            last_sig = sig;
            chrome_dirty = 1;
        }
        if (chrome_dirty > 0) {
            chrome_dirty--;
            ui_clear_all(s_page, COL_BG);
            const text_canvas_t ptc = {
                .pixels = s_page, .width = BSP_LCD_H_RES, .height = BSP_LCD_V_RES};
            if (on_session_page) {
                session_page_draw(s_page, &ptc, &s_model, focus, state, t, now_unix);
            } else {
                admin_page_draw(s_page, &ptc, &s_model, t, now_unix);
            }
            memcpy(fb, s_page,
                   (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t));
        }

      if (on_session_page) {
        clawd_rect_t box = clawd_bounds(&params);
        /*
         * **合成区必须夹在一条安全带里。**
         * 它每帧都被整块覆盖，一旦上缘越过 subagent 圆点、下缘压到项目名，
         * 那两样东西就会被每帧擦掉——表现为"圆点只剩半个""项目名不显示"，
         * 看起来像绘制没画上去，实际是画上去了又被盖掉。
         * 越界的部分是包围盒的空白外扩，裁掉不影响人物本身。
         */
        if (box.y < SPRITE_BAND_TOP) {
            box.h -= (SPRITE_BAND_TOP - box.y);
            box.y = SPRITE_BAND_TOP;
        }
        if (box.y + box.h > SPRITE_BAND_BOTTOM) box.h = SPRITE_BAND_BOTTOM - box.y;
        if (box.w > s_scratch_w) box.w = s_scratch_w;
        if (box.h > s_scratch_h) box.h = s_scratch_h;
        if (box.h <= 0 || box.w <= 0) goto skip_sprite;

        /* 1) 在 scratch 里擦干净并画好整块内容 */
        for (int i = 0; i < box.w * box.h; i++) s_scratch[i] = COL_BG;
        const clawd_canvas_t scratch_canvas = {
            .pixels = s_scratch, .width = box.w, .height = box.h};
        clawd_draw_t local = params;
        local.center_x = params.center_x - box.x;
        local.baseline_y = params.baseline_y - box.y;
        clawd_draw(&scratch_canvas, &local);

        /* 2) 整块拷进帧缓冲——一次线性写入，不暴露中间态 */
        for (int row = 0; row < box.h; row++) {
            const int dst_y = box.y + row;
            if (dst_y < 0 || dst_y >= BSP_LCD_V_RES) continue;
            memcpy(fb + (size_t)dst_y * BSP_LCD_H_RES + box.x,
                   s_scratch + (size_t)row * box.w, (size_t)box.w * sizeof(uint16_t));
        }
      skip_sprite:;
      }

        /* 单帧缓冲 + bounce：写入即显示，不需要 flush/交换 */

        if (ring != last_active || (uint32_t)(t - report_at) > 10000) {
            last_active = ring;
            report_at = t;
            printf("  页=%s 在环=%d/%d 焦点=%s 状态=%d sub=%d/%d ctx=%.0f%% 5h=%.0f%% wk=%.0f%% 帧=%lums 丢帧=%lu\n",
                   on_session_page ? "会话" : "管理",
                   ring, model_active_count(&s_model), focus ? focus->name : "-", (int)state,
                   focus ? model_sub_done(focus) : 0, focus ? model_sub_total(focus) : 0,
                   (double)(focus ? focus->ctx_pct : -1.0f),
                   (double)s_model.limits.five_hour.pct,
                   (double)s_model.limits.seven_day.pct,
                   (unsigned long)s_frame_ms, (unsigned long)link_dropped());
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
