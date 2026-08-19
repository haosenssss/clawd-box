/*
 * 极简位图文字渲染。5×7 点阵，整数倍放大。
 *
 * 不引 LVGL：整个界面只需要会话名、状态词、百分比和几个标签，
 * 为此拖进一整套 GUI 框架不划算。这里约 200 行、零依赖、直接写帧缓冲。
 *
 * 只覆盖 ASCII 0x20-0x7E。会话名实际都是 ASCII（weixue-de / hquant-57），
 * 官方状态词也是英文，够用。
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint16_t *pixels;
    int width;
    int height;
} text_canvas_t;

/** 单个字符的像素宽（含 1 列字距），scale 倍放大后。 */
#define TEXT_ADVANCE(scale) (6 * (scale))
#define TEXT_HEIGHT(scale) (7 * (scale))

/** 字符串按 scale 倍放大后的像素宽度。 */
int text_width(const char *s, int scale);

/** 左上角对齐绘制。返回结束位置的 x。 */
int text_draw(const text_canvas_t *c, int x, int y, const char *s, int scale,
              uint16_t color);

/** 水平居中绘制。 */
void text_draw_center(const text_canvas_t *c, int cx, int y, const char *s, int scale,
                      uint16_t color);
