/*
 * 视觉常量：配色、字号、版面。
 *
 * 全部集中在这里，因为它们**互相约束**——改精灵的 px/单位就必须同步改
 * 额度条的宽度和左右两列的位置，散在各处一定会改漏。
 */
#pragma once

#include "sprite/clawd.h"

/* ---- 官方配色（Claude Code theme.ts / Anthropic 品牌 token）---- */
/* 纯黑。Clawd 的官方背景 token clawd_background 就是 rgb(0,0,0)；
 * 之前用 #141413 是把 Anthropic 的页面背景色用错了地方。
 * 纯黑还让橙色主体和 IPS 屏的对比度都达到最好。 */
#define COL_BG clawd_rgb565(0x00, 0x00, 0x00)
#define COL_BODY clawd_rgb565(0xD7, 0x77, 0x57)    /* clawd_body */
#define COL_EYE clawd_rgb565(0x00, 0x00, 0x00)     /* clawd_background */
#define COL_TEXT clawd_rgb565(0xF0, 0xEE, 0xE6)    /* --swatch--ivory-medium，数据用 */
#define COL_NAME clawd_rgb565(0xB0, 0xAE, 0xA5)    /* --swatch--cloud-medium，项目名 */
#define COL_VERB clawd_rgb565(0x78, 0x76, 0x70)    /* 状态词，更暗一档 */
#define COL_RESET clawd_rgb565(0x87, 0x86, 0x7F)   /* --swatch--cloud-dark，重置时间 */
#define COL_DIM clawd_rgb565(0x99, 0x99, 0x99)     /* inactive */
#define COL_SUBTLE clawd_rgb565(0x50, 0x50, 0x50)  /* subtle */
#define COL_FILL clawd_rgb565(0x57, 0x69, 0xF7)    /* rate_limit_fill */
#define COL_EMPTY clawd_rgb565(0x27, 0x2F, 0x6F)   /* rate_limit_empty */
#define COL_WARN clawd_rgb565(0xFF, 0xC1, 0x07)    /* warning */
#define COL_CRIT clawd_rgb565(0xFF, 0x6B, 0x80)    /* error */
#define COL_OK clawd_rgb565(0x4E, 0xBA, 0x65)      /* success */

#define TARGET_FPS 30
#define FRAME_MS (1000 / TARGET_FPS)
#define ROTATE_MS 7000

#define SPRITE_SCALE 15.2f   /* 15 单位宽 -> 228px，与额度条同宽同轴 */
#define SPRITE_BASELINE_Y 244

/*
 * 精灵合成区的上下边界。合成区每帧整块覆盖，所以它**绝不能碰到**
 * 上面的 subagent 圆点和下面的项目名——碰到就是每帧把它们擦一次。
 * 人物本体在 54..282 之间（含 ±24px 重力位移），这条带子刚好把它包住。
 */
#define SPRITE_BAND_TOP 50
#define SPRITE_BAND_BOTTOM 278

/* ---- 布局 ---- */
#define DOTS_Y 38
#define DOT_R 7
#define DOT_GAP 26
/* 项目名占**两行**的排版：第一行基线、行距、之后才是状态词。
 * 长项目名靠缩字号是治不好的——横向就那么宽，必须折行。 */
#define NAME_Y 286
#define NAME_LINE_H 22
#define VERB_Y 332           /* 状态词 */
#define BAR_LABEL_CX 50       /* 标签列中心（0..条子左缘 之间居中） */
#define BAR_X 126              /* = 精灵左边缘，两者必须等宽同轴 */
/*
 * 图形宽度是被**右侧两列**卡死的，不是想多宽就多宽：
 * 条子右缘之后还要放下百分比和倒计时，挤了就会把倒计时顶出屏幕。
 * 240 = 15 单位 x 16，右边留出 120px 给两列 + 边距，是能同时保住
 * "图标够大"和"重置时间显示完整"的那个平衡点。
 */
#define BAR_W 228
#define BAR_H 16
/* 百分比列**右对齐**：这一列的值宽度不一（5% / 42% / 100%），
 * 左对齐会让各行的 % 号错开成锯齿。右边缘对齐后 % 永远在同一条竖线上。 */
#define BAR_PCT_R 408
/* 最右一列**右对齐到这里**。左对齐的话内容一长就顶出屏幕
 * （"roomy" 五个字母就溢出了），右对齐则长短都落在同一条竖线上。 */
#define BAR_RIGHT_EDGE 464
#define BAR_Y0 358
#define BAR_DY 42
#define BAR_ROWS 3
/* 文字 7 单位高，比条子矮，垂直居中到条子里 */
#define BAR_TEXT_DY ((BAR_H - 7 * TEXT_SCALE) / 2)
#define TEXT_SCALE 2
