/*
 * 把板子上的精灵渲染器编到 Mac 上跑，直接导出画面。
 *
 * 之前每一版动效都是"改完烧进去，问用户好不好看"——等于盲画。
 * 同一份 clawd.c 在这里离线渲染成图，改一版看一版，
 * 比在硬件上盲改快一个数量级，也不会再出现"我说的和你看到的不一样"。
 */
#include "sprite/clawd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef W
#define W 240
#endif
#ifndef H
#define H 240
#endif
#define COLS 6

static void rgb565_to_rgb888(uint16_t c, unsigned char *out)
{
    const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    out[0] = (unsigned char)((r * 255 + 15) / 31);
    out[1] = (unsigned char)((g * 255 + 31) / 63);
    out[2] = (unsigned char)((b * 255 + 15) / 31);
}

int main(int argc, char **argv)
{
    const char *out_path = argc > 1 ? argv[1] : "/tmp/clawd_preview.raw";
    /* 第二个参数：只渲染这一个状态（0..4），列数翻倍看细节 */
    const int only = argc > 2 ? atoi(argv[2]) : -1;

    const clawd_state_t states[] = {CLAWD_WORKING, CLAWD_DONE, CLAWD_WAITING, CLAWD_IDLE,
                                    CLAWD_SLEEPING};
    const int nstates = (int)(sizeof(states) / sizeof(states[0]));
    /* 每个状态取 6 个时间点，覆盖一个完整周期 */
    /* **每个状态按自己的周期采样。** 统一用 1 秒窗口的话，
     * 9 秒的张望循环在里面根本看不出动静，会误判成"这个状态没动画"。 */
    const uint32_t period[] = {2080, 1000, 6000, 9000, 4500};

    const int cols = only >= 0 ? COLS * 2 : COLS;
    const int rows = only >= 0 ? 1 : nstates;
    const int sheet_w = W * cols, sheet_h = H * rows;
    uint16_t *fb = calloc((size_t)W * H, sizeof(uint16_t));
    unsigned char *sheet = calloc((size_t)sheet_w * sheet_h * 3, 1);

    for (int r = 0; r < rows; r++) {
        const int s = only >= 0 ? only : r;
        for (int c = 0; c < cols; c++) {
            for (int i = 0; i < W * H; i++) fb[i] = 0; /* 纯黑背景，和板子一致 */

            const clawd_canvas_t canvas = {.pixels = fb, .width = W, .height = H};
            const clawd_draw_t p = {
                .center_x = W / 2,
                .baseline_y = 190,
                .px_per_unit = 14.9f * (float)W / 480.0f * 2.0f, /* 与板子同比例 */
                .state = states[s],
                .elapsed_ms = period[s] * (uint32_t)c / (uint32_t)cols,
                .body_color = clawd_rgb565(0xD7, 0x77, 0x57),
                .eye_color = 0,
                .shadow_color = 0,
            };
            clawd_draw(&canvas, &p);

            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    const int sx = c * W + x, sy = r * H + y;
                    rgb565_to_rgb888(fb[y * W + x], sheet + ((size_t)sy * sheet_w + sx) * 3);
                }
            }
        }
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "%d %d\n", sheet_w, sheet_h);
    fwrite(sheet, 1, (size_t)sheet_w * sheet_h * 3, f);
    fclose(f);
    printf("%d %d %s\n", sheet_w, sheet_h, out_path);
    return 0;
}
