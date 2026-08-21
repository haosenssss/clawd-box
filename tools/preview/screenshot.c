/*
 * 整屏截图。
 *
 * 把板子的页面渲染（pages.c）原样跑在本机，输出 480x480 的真实画面——
 * 和屏幕上逐像素一致，不是重画的示意图。README 里的图靠它出。
 *
 * 需要 tools/native-stub 里的两个桩：board_config.h 会拉 ESP-IDF 的
 * driver/gpio.h，而我们只用到它里面的分辨率常量。
 *
 *   clang -O2 -I firmware/main -I firmware/main/ui -I tools/native-stub \
 *         -o /tmp/shot tools/preview/screenshot.c \
 *         firmware/main/ui/pages.c firmware/main/sprite/clawd.c \
 *         firmware/main/sprite/text.c firmware/main/model/sessions.c -lm
 *   /tmp/shot out.raw [毫秒]
 */
#include "ui/pages.h"
#include "bsp/board_config.h"
#include "ui/theme.h"
#include "model/sessions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    const int W = BSP_LCD_H_RES, H = BSP_LCD_V_RES;
    uint16_t *fb = calloc((size_t)W * H, 2);
    model_t m; memset(&m, 0, sizeof(m));
    uint32_t t = 90000;
    m.linked = true; m.last_frame_ms = t;
    m.limits.five_hour.pct = 34.0f;
    m.limits.seven_day.pct = 62.0f;
    m.limits.five_hour.resets_at = 1787240000; m.limits.seven_day.resets_at = 1787600000;
    m.host_unix_sec = 1787230000; m.host_sync_ms = t;
    session_t *s = model_touch_session(&m, "a1", t);
    strcpy(s->name, "clawd-box"); s->status = SESS_BUSY; s->status_since_ms = t;
    s->ctx_pct = 47.0f;
    model_sub_start(s, "g1"); model_sub_start(s, "g2"); model_sub_start(s, "g3");
    model_sub_stop(s, "g1"); model_sub_stop(s, "g2");
    ui_clear_all(fb, COL_BG);
    const text_canvas_t tc = {.pixels = fb, .width = W, .height = H};
    const uint32_t now = argc > 2 ? (uint32_t)atoi(argv[2]) : t;
    session_page_draw(fb, &tc, &m, s, CLAWD_WORKING, now, m.host_unix_sec);

    /* 精灵是 main.c 单独合成再拷进帧缓冲的，session_page_draw() 只画外围。
     * 这里直接画进同一块 fb，参数和 main.c 保持一致。 */
    const clawd_canvas_t cv = {.pixels = fb, .width = W, .height = H};
    const clawd_draw_t params = {
        .center_x = BSP_LCD_H_RES / 2,
        .baseline_y = SPRITE_BASELINE_Y,
        .px_per_unit = SPRITE_SCALE,
        .state = CLAWD_WORKING,
        .elapsed_ms = now,
        .body_color = COL_BODY,
        .eye_color = COL_EYE,
        .shadow_color = COL_BG,
    };
    clawd_draw(&cv, &params);
    FILE *f = fopen(argv[1], "wb");
    fprintf(f, "%d %d 1\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t v = fb[i];
        unsigned char p[3] = {(unsigned char)((((v>>11)&0x1F)*255+15)/31),
                              (unsigned char)((((v>>5)&0x3F)*255+31)/63),
                              (unsigned char)(((v&0x1F)*255+15)/31)};
        fwrite(p, 1, 3, f);
    }
    fclose(f); printf("%dx%d -> %s\n", W, H, argv[1]);
    return 0;
}
