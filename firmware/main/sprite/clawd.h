/*
 * Clawd —— Claude Code 官方吉祥物的参数化渲染器。
 *
 * 不用位图。角色由 10 个矩形构成，定义在 15×16 单位网格上；
 * 缩放是自由参数，所以任意尺寸都原生清晰，且零 flash 素材。
 *
 * 几何取自社区的 clawd-static-base.svg，已与 Claude Code 终端里的官方
 * Clawd（src/components/LogoV2/Clawd.tsx 的四分块字符）交叉验证：
 *   臂/身宽比 1.36 vs 1.33 · 四脚位置 0.20/0.33/0.60/0.73 vs 0.22/0.33/0.61/0.72
 *   眼心 0.30/0.70 vs 0.28/0.67
 *
 * 动画曲线逐条移植自 rullerzhou-afk/clawd-on-desk 的 SVG 关键帧——
 * 那些动画本来就是对同名矩形组做 CSS transform，所以是等价移植而非"看着像"。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 单位网格尺寸 */
#define CLAWD_UNIT_W 15
#define CLAWD_UNIT_H 16

/* 官方配色 —— Claude Code theme.ts 的 clawd_body / clawd_background */
#define CLAWD_BODY_R 0xD7
#define CLAWD_BODY_G 0x77
#define CLAWD_BODY_B 0x57

/** 主 agent 的状态。**用动作表达，不用图标。** */
typedef enum {
    CLAWD_IDLE = 0,    /* 极慢呼吸 + 偶尔眨眼/左右看 */
    CLAWD_WORKING,     /* 伏案打字：双手互质频率敲击 + 身体轻快弹跳 */
    CLAWD_DONE,        /* 举手庆祝：squash-and-stretch 跳跃，影子反向耦合 */
    CLAWD_WAITING,     /* 举灯泡吃力发抖 —— 需要你动手 */
    CLAWD_SLEEPING,    /* 无活跃会话时的待机 */
} clawd_state_t;

/** 绘制目标。RGB565，行优先。 */
typedef struct {
    uint16_t *pixels;
    int width;
    int height;
} clawd_canvas_t;

/** 一次绘制的布局与状态。 */
typedef struct {
    /* 精灵中心在画布上的位置（像素） */
    int center_x;
    /* 脚底基线的 y 坐标（像素）——动画以脚底为变换原点，所以用基线而非中心 */
    int baseline_y;
    /* 每单位多少像素。会话页用 15（→225×240），管理页用 8。 */
    float px_per_unit;

    clawd_state_t state;
    /* 自进入当前状态起经过的毫秒数，驱动所有周期 */
    uint32_t elapsed_ms;

    /* 重力位移（像素），由 IMU 弹簧物理给出。上限应由调用方限制在 ±24。 */
    /** 整体旋转（弧度），绕脚底支点。沿边框爬行时用来让脚踩在边上。 */
    float world_rot;
    float tilt_x;
    float tilt_y;

    uint16_t body_color;
    uint16_t eye_color;
    uint16_t shadow_color;
} clawd_draw_t;

/** 把 8-8-8 转成 RGB565。 */
static inline uint16_t clawd_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** 画一只 Clawd。不清屏——调用方负责背景。 */
void clawd_draw(const clawd_canvas_t *canvas, const clawd_draw_t *params);

typedef struct {
    int x, y, w, h;
} clawd_rect_t;

/**
 * 该次绘制会触及的像素包围盒（已含各状态的最大位移、旋转和重力偏移余量）。
 * 用它做脏矩形清屏——全屏清一次 480×480 要 20ms 以上，只清包围盒能省掉大半。
 */
clawd_rect_t clawd_bounds(const clawd_draw_t *params);

/**
 * 当前状态的一个完整动画周期长度（毫秒）。
 * CLAWD_DONE 是一次性的，播完应由调用方切回 IDLE/WORKING。
 */
uint32_t clawd_cycle_ms(clawd_state_t state);

/** CLAWD_DONE 是否已播完。 */
bool clawd_done_finished(uint32_t elapsed_ms);
