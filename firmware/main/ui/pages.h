/*
 * 页面渲染：会话页与管理页。
 *
 * 只负责把 model 画出来，不做任何状态判断——该显示谁由 pager 决定。
 */
#pragma once

#include "model/sessions.h"
#include "sprite/clawd.h"
#include "sprite/text.h"
#include <stdint.h>

/** 会话页的静态部分（顶部圆点、名字、状态词、底部额度条）。精灵由主循环画。 */
void session_page_draw(uint16_t *fb, const text_canvas_t *tc, const model_t *m,
                       const session_t *focus, clawd_state_t state, uint32_t now_ms_,
                       int64_t now_unix);

/** 会话页内容签名：变了才需要重画。 */
uint32_t session_page_signature(const model_t *m, const session_t *focus,
                                clawd_state_t state, uint32_t now_ms_);

/* 管理页列表的行几何。pager 要靠它把点按的 y 映射回是哪一行。 */
#define ADMIN_ROW_Y0 84
#define ADMIN_ROW_DY 38
#define ADMIN_ROW_MAX 6

/** 管理页：全部会话一览 + 额度总览。 */
void admin_page_draw(uint16_t *fb, const text_canvas_t *tc, const model_t *m,
                     uint32_t now_ms_, int64_t now_unix);

uint32_t admin_page_signature(const model_t *m, int64_t now_unix);

/** 睡眠态的 ZZZ。位置由主循环给（要跟着精灵走）。 */
void draw_zzz(const text_canvas_t *tc, int x, int y, uint32_t t);

/** 整屏填充。切换页面时用。 */
void ui_clear_all(uint16_t *fb, uint16_t c);
