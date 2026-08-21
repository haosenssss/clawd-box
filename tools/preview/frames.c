/*
 * 逐帧导出，用来生成 README 里的动图。
 *
 * 和 preview.c 共用同一份 clawd.c —— 导出的就是板子上逐像素显示的内容，
 * 不是重新画的示意图。README 里的动图必须是真的，
 * 否则读者看到的和拿到手的对不上。
 *
 *   frames <out.raw> <状态 0..4> <周期ms> <帧数>
 *   输出: "宽 高 帧数\n" 后接 N 帧 RGB888
 */
#include "sprite/clawd.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 9) {
        fprintf(stderr,
                "用法: frames <out.raw> <状态0..4> <周期ms> <帧数> <宽> <高> <每单位像素> <基线y>\n"
                "\n"
                "**按最终尺寸直接渲染，不要渲染大图再缩。** 缩放会把平涂色块插值出\n"
                "几百种中间调，把调色板槽位占满，反而挤掉像青色彩纸这种像素少但\n"
                "关键的颜色。渲染器本身带抗锯齿，直接出小图更干净、体积也更小。\n");
        return 1;
    }
    const char *out_path = argv[1];
    const int state = atoi(argv[2]);
    const uint32_t period = (uint32_t)atoi(argv[3]);
    const int n = atoi(argv[4]);
    const int W = atoi(argv[5]);
    const int H = atoi(argv[6]);
    const float scale = (float)atof(argv[7]);
    const int baseline = atoi(argv[8]);

    uint16_t *fb = calloc((size_t)W * H, sizeof(uint16_t));
    unsigned char *rgb = calloc((size_t)W * H * 3, 1);
    FILE *f = fopen(out_path, "wb");
    if (fb == NULL || rgb == NULL || f == NULL) return 1;
    fprintf(f, "%d %d %d\n", W, H, n);

    for (int i = 0; i < n; i++) {
        for (int k = 0; k < W * H; k++) fb[k] = 0;
        const clawd_canvas_t cv = {.pixels = fb, .width = W, .height = H};
        const clawd_draw_t p = {
            .center_x = W / 2,
            .baseline_y = baseline,
            .px_per_unit = scale,
            .state = (clawd_state_t)state,
            .elapsed_ms = period * (uint32_t)i / (uint32_t)n,
            .body_color = clawd_rgb565(0xD7, 0x77, 0x57),
            .eye_color = 0,
            .shadow_color = 0,
        };
        clawd_draw(&cv, &p);
        for (int k = 0; k < W * H; k++) {
            const uint16_t v = fb[k];
            rgb[k * 3 + 0] = (unsigned char)((((v >> 11) & 0x1F) * 255 + 15) / 31);
            rgb[k * 3 + 1] = (unsigned char)((((v >> 5) & 0x3F) * 255 + 31) / 63);
            rgb[k * 3 + 2] = (unsigned char)(((v & 0x1F) * 255 + 15) / 31);
        }
        fwrite(rgb, 1, (size_t)W * H * 3, f);
    }
    fclose(f);
    printf("%dx%d %d 帧 -> %s\n", W, H, n, out_path);
    return 0;
}
