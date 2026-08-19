#include "clawd.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * 几何：15×16 单位网格上的 10 个矩形
 * ------------------------------------------------------------------ */

typedef struct {
    float x, y, w, h;
} rect_t;

static const rect_t R_TORSO = {2.0f, 6.0f, 11.0f, 7.0f};
static const rect_t R_ARM_L = {0.0f, 9.0f, 2.0f, 2.0f};
static const rect_t R_ARM_R = {13.0f, 9.0f, 2.0f, 2.0f};
static const rect_t R_LEGS[4] = {
    {3.0f, 13.0f, 1.0f, 2.0f},
    {5.0f, 13.0f, 1.0f, 2.0f},
    {9.0f, 13.0f, 1.0f, 2.0f},
    {11.0f, 13.0f, 1.0f, 2.0f},
};
static const rect_t R_EYE_L = {4.0f, 8.0f, 1.0f, 2.0f};
static const rect_t R_EYE_R = {10.0f, 8.0f, 1.0f, 2.0f};

/*
 * **腿的几何是随状态变的，不是固定的。**
 * 官方每个状态的 SVG 里腿都不一样：站着发呆时腿是加长的（y=11 h=4，
 * 身体显得立起来、有精神），干活/庆祝时是短腿（y=13 h=2）。
 * 我一开始全用短腿，结果站姿看着蔫，这是"不可爱"的一大来源。
 */
static const rect_t R_LEGS_TALL[4] = {
    {3.0f, 11.0f, 1.0f, 4.0f},
    {5.0f, 11.0f, 1.0f, 4.0f},
    {9.0f, 11.0f, 1.0f, 4.0f},
    {11.0f, 11.0f, 1.0f, 4.0f},
};

/*
 * 睡觉是**另一套完全不同的身体**：摊平的胖躯干、贴地的手、
 * 从身后翘起的小短腿、两道细线眼。不是"站着的它闭上眼"，
 * 而是"它化成一摊"——这个姿势本身就是全部的萌点，几何不换就没有。
 */
static const rect_t R_SLEEP_TORSO = {1.0f, 10.0f, 13.0f, 5.0f};
static const rect_t R_SLEEP_ARM_L = {-1.0f, 13.0f, 2.0f, 2.0f};
static const rect_t R_SLEEP_ARM_R = {14.0f, 13.0f, 2.0f, 2.0f};
static const rect_t R_SLEEP_LEGS[4] = {
    {3.0f, 9.0f, 1.0f, 1.0f},
    {5.0f, 9.0f, 1.0f, 1.0f},
    {9.0f, 9.0f, 1.0f, 1.0f},
    {11.0f, 9.0f, 1.0f, 1.0f},
};
static const rect_t R_SLEEP_EYE_L = {3.5f, 12.5f, 2.0f, 0.4f};
static const rect_t R_SLEEP_EYE_R = {9.5f, 12.5f, 2.0f, 0.4f};

/* 手臂旋转的支点在它与身体相接的内侧 */
static const float ARM_L_PIVOT_X = 2.0f, ARM_L_PIVOT_Y = 10.0f;
static const float ARM_R_PIVOT_X = 13.0f, ARM_R_PIVOT_Y = 10.0f;

/* 身体整体变换的原点在脚底中线——squash-and-stretch 必须绕脚底做 */
static const float BODY_PIVOT_X = 7.5f;
static const float BODY_PIVOT_Y = 15.0f;

/* ------------------------------------------------------------------ *
 * 关键帧求值：给定 0..1 的相位和一张 {位置, 值} 表，线性插值
 * ------------------------------------------------------------------ */

typedef struct {
    float at;  /* 0..1 */
    float value;
} key_t;

static float ease_keys(const key_t *keys, size_t n, float phase)
{
    if (n == 0) return 0.0f;
    if (phase <= keys[0].at) return keys[0].value;
    for (size_t i = 1; i < n; i++) {
        if (phase <= keys[i].at) {
            const float span = keys[i].at - keys[i - 1].at;
            if (span <= 0.0f) return keys[i].value;
            const float t = (phase - keys[i - 1].at) / span;
            /* 平滑一点，避免线性插值看起来太机械 */
            const float s = t * t * (3.0f - 2.0f * t);
            return keys[i - 1].value + (keys[i].value - keys[i - 1].value) * s;
        }
    }
    return keys[n - 1].value;
}

/** step-end：不插值，直接跳变。像素画风格的闪烁/眨眼用它。 */
__attribute__((unused)) static float step_keys(const key_t *keys, size_t n, float phase)
{
    float v = n > 0 ? keys[0].value : 0.0f;
    for (size_t i = 0; i < n; i++) {
        if (phase >= keys[i].at) v = keys[i].value;
    }
    return v;
}

static float phase_of(uint32_t elapsed_ms, uint32_t period_ms)
{
    if (period_ms == 0) return 0.0f;
    return (float)(elapsed_ms % period_ms) / (float)period_ms;
}

/* ------------------------------------------------------------------ *
 * 光栅化：凸四边形扫描线填充（旋转后的矩形就是四边形）
 * ------------------------------------------------------------------ */

typedef struct {
    float x, y;
} pt_t;

/**
 * RGB565 混色。抗锯齿的边缘像素靠它和背景过渡。
 */
static uint16_t blend565(uint16_t dst, uint16_t src, float a)
{
    if (a >= 0.996f) return src;
    if (a <= 0.004f) return dst;
    const int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    const int sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    const int r = dr + (int)lroundf((float)(sr - dr) * a);
    const int g = dg + (int)lroundf((float)(sg - dg) * a);
    const int b = db + (int)lroundf((float)(sb - db) * a);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/*
 * 凸四边形填充，**带抗锯齿**。
 *
 * 这是"动作看着假"的真正原因，比曲线本身重要得多：
 * 整像素填充时，一条 2 单位宽的手臂在旋转过程中边缘只能一格一格地跳，
 * 于是每帧都在"啪"地挪一整个像素——曲线再准，画出来也是硬跳。
 * 人眼对边缘的亚像素位移极其敏感，抗锯齿一开，同样的关键帧立刻就"活"了。
 *
 * 做法：纵向 4 次子采样求每行的覆盖跨度，横向用精确的分数覆盖率，
 * 然后和已有像素混色。代价是每个四边形多几倍的算术，
 * 但精灵总共才十来个四边形，换来的平滑度完全值得。
 */
#define AA_SUBROWS 3
static float s_cov[640];

/** 一维覆盖率：区间 [lo,hi] 落在整数格 i 上的比例。 */
static inline float span_cov(float lo, float hi, int i)
{
    const float l = lo > (float)i ? lo : (float)i;
    const float r = hi < (float)(i + 1) ? hi : (float)(i + 1);
    return r > l ? r - l : 0.0f;
}

/*
 * 轴对齐矩形的快速路径。
 *
 * **躯干和腿占了绝大部分像素，而它们多数时候根本没有旋转。**
 * 对这些矩形做逐行子采样纯属浪费——横竖两个方向各算一次分数覆盖率，
 * 乘起来就是精确解，既没有采样误差又快得多。
 */
static bool fill_axis_rect(const clawd_canvas_t *c, const pt_t q[4], uint16_t color)
{
    const float eps = 0.02f;
    if (fabsf(q[0].y - q[1].y) > eps || fabsf(q[2].y - q[3].y) > eps ||
        fabsf(q[0].x - q[3].x) > eps || fabsf(q[1].x - q[2].x) > eps) {
        return false;
    }
    float lox = q[0].x, hix = q[1].x, loy = q[0].y, hiy = q[2].y;
    if (hix < lox) { const float t = lox; lox = hix; hix = t; }
    if (hiy < loy) { const float t = loy; loy = hiy; hiy = t; }

    int y0 = (int)floorf(loy), y1 = (int)ceilf(hiy);
    int x0 = (int)floorf(lox), x1 = (int)ceilf(hix);
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > c->height) y1 = c->height;
    if (x1 > c->width) x1 = c->width;
    if (y1 <= y0 || x1 <= x0) return true;

    for (int y = y0; y < y1; y++) {
        const float vy = span_cov(loy, hiy, y);
        if (vy <= 0.004f) continue;
        uint16_t *row = c->pixels + (size_t)y * c->width;
        if (vy > 0.996f) {
            /* 整行覆盖：中间整格直接写，只有左右两端要混色 */
            for (int x = x0; x < x1; x++) {
                const float a = span_cov(lox, hix, x);
                if (a <= 0.004f) continue;
                row[x] = (a > 0.996f) ? color : blend565(row[x], color, a);
            }
            continue;
        }
        for (int x = x0; x < x1; x++) {
            const float a = vy * span_cov(lox, hix, x);
            if (a <= 0.004f) continue;
            row[x] = blend565(row[x], color, a > 1.0f ? 1.0f : a);
        }
    }
    return true;
}

static void fill_quad(const clawd_canvas_t *c, const pt_t q[4], uint16_t color)
{
    if (fill_axis_rect(c, q, color)) return;
    float miny = q[0].y, maxy = q[0].y, minx = q[0].x, maxx = q[0].x;
    for (int i = 1; i < 4; i++) {
        if (q[i].y < miny) miny = q[i].y;
        if (q[i].y > maxy) maxy = q[i].y;
        if (q[i].x < minx) minx = q[i].x;
        if (q[i].x > maxx) maxx = q[i].x;
    }
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > c->height) y1 = c->height;
    if (x1 > c->width) x1 = c->width;
    if (y1 <= y0 || x1 <= x0) return;
    if (x1 - x0 > (int)(sizeof(s_cov) / sizeof(s_cov[0]))) x1 = x0 + (int)(sizeof(s_cov) / sizeof(s_cov[0]));

    const float share = 1.0f / (float)AA_SUBROWS;

    for (int y = y0; y < y1; y++) {
        for (int i = x0; i < x1; i++) s_cov[i - x0] = 0.0f;
        bool any = false;

        for (int sub = 0; sub < AA_SUBROWS; sub++) {
            const float sy = (float)y + ((float)sub + 0.5f) * share;

            /* 凸多边形与一条水平线最多交于一个区间，取交点的最小/最大即可 */
            float lo = 1e9f, hi = -1e9f;
            for (int e = 0; e < 4; e++) {
                const pt_t *p = &q[e];
                const pt_t *n = &q[(e + 1) & 3];
                if ((p->y <= sy) == (n->y <= sy)) continue;
                const float dy = n->y - p->y;
                if (dy == 0.0f) continue;
                const float x = p->x + (n->x - p->x) * (sy - p->y) / dy;
                if (x < lo) lo = x;
                if (x > hi) hi = x;
            }
            if (hi <= lo) continue;
            if (lo < (float)x0) lo = (float)x0;
            if (hi > (float)x1) hi = (float)x1;
            if (hi <= lo) continue;
            any = true;

            /* 横向精确覆盖率：两端按分数计，中间整格 */
            const int ilo = (int)floorf(lo), ihi = (int)ceilf(hi);
            for (int x = ilo; x < ihi && x < x1; x++) {
                if (x < x0) continue;
                s_cov[x - x0] += span_cov(lo, hi, x) * share;
            }
        }
        if (!any) continue;

        uint16_t *row = c->pixels + (size_t)y * c->width;
        for (int x = x0; x < x1; x++) {
            const float a = s_cov[x - x0];
            if (a <= 0.004f) continue;
            row[x] = blend565(row[x], color, a > 1.0f ? 1.0f : a);
        }
    }
}

/* ------------------------------------------------------------------ *
 * 变换：单位坐标 → 画布像素
 * ------------------------------------------------------------------ */

typedef struct {
    /* 身体整体（绕脚底） */
    float body_dy;     /* 单位 */
    float body_sx, body_sy;
    float body_rot;    /* 弧度 */
    /* 手臂（绕各自支点） */
    float arm_l_rot;
    float arm_r_rot;
    float arm_r_dy;    /* 举起时的额外抬升 */
    /* 腿部颤抖 */
    float leg_rot[4];
    /* 眼睛纵向压缩，1=睁开 0=闭合 */
    float eye_scale_y;
    float eye_dx;
    float eye_dy;
    /* 眼睛形态：0=方块眼 1=^^ 笑眼 2=睡觉的细线 */
    int eye_mode;
    /* 身体几何：0=常规 1=长腿站姿 2=摊平睡姿 */
    int body_form;
    /* 影子 */
    float shadow_sx;
    float shadow_alpha; /* 0..1，这里用来在两档颜色间取舍 */
} pose_t;

typedef struct {
    float ox, oy;   /* 画布上单位原点（精灵左上角）的像素坐标 */
    float scale;    /* 每单位像素数 */
} view_t;

static pt_t to_px(const view_t *v, float ux, float uy)
{
    return (pt_t){v->ox + ux * v->scale, v->oy + uy * v->scale};
}

/** 对一个单位矩形施加 [绕 pivot 的旋转+缩放] 后输出画布四边形。 */
static void transform_rect(const view_t *v, const rect_t *r, float pivot_x, float pivot_y,
                           float rot, float sx, float sy, float dx, float dy, pt_t out[4])
{
    const float cs = cosf(rot), sn = sinf(rot);
    const float cx[4] = {r->x, r->x + r->w, r->x + r->w, r->x};
    const float cy[4] = {r->y, r->y, r->y + r->h, r->y + r->h};
    for (int i = 0; i < 4; i++) {
        float px = (cx[i] - pivot_x) * sx;
        float py = (cy[i] - pivot_y) * sy;
        const float rx = px * cs - py * sn;
        const float ry = px * sn + py * cs;
        out[i] = to_px(v, pivot_x + rx + dx, pivot_y + ry + dy);
    }
}


/* ------------------------------------------------------------------ *
 * 小道具
 *
 * **不画大件家具。** 试过在腿前面摆一块整幅宽的键盘，结果就是一根丑陋的
 * 灰色横条把人物腰斩——15x16 的格子里塞不下一张桌子，塞进去只会盖住角色本身。
 * 道具要小、要在人物轮廓之外、要能一眼读懂：火花、灯泡、Z。
 * ------------------------------------------------------------------ */

#define BIT_COL clawd_rgb565(0x40, 0xC4, 0xFF)
#define SPARK_A clawd_rgb565(0xFF, 0xD7, 0x00)
#define SPARK_B clawd_rgb565(0xFF, 0xF5, 0x9D)
#define BULB_ON clawd_rgb565(0xFF, 0xD4, 0x00)
#define BULB_OFF clawd_rgb565(0x8A, 0x74, 0x28)
#define BULB_EDGE_ON clawd_rgb565(0xFF, 0xB0, 0x00)
#define BULB_EDGE_OFF clawd_rgb565(0x72, 0x5B, 0x20)
#define ZZZ_COL clawd_rgb565(0xB0, 0xBE, 0xC5)

static void put_unit_rect(const clawd_canvas_t *canvas, const view_t *v, float x, float y,
                          float w, float h, float bdy, uint16_t color)
{
    pt_t quad[4];
    const rect_t r = {x, y, w, h};
    transform_rect(v, &r, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, bdy, quad);
    fill_quad(canvas, quad, color);
}

/* --- 干活：思绪从头顶飘出去 --- */
#define BIT_COUNT 4
static const float BIT_X[BIT_COUNT] = {-1.4f, 15.6f, -0.9f, 15.1f};
static const uint32_t BIT_DELAY[BIT_COUNT] = {0, 500, 1000, 1500};
#define BIT_PERIOD 2000

static void props_working(const clawd_canvas_t *canvas, const view_t *v, uint32_t t, float bdy)
{
    for (int i = 0; i < BIT_COUNT; i++) {
        const uint32_t ph = (t + BIT_DELAY[i]) % BIT_PERIOD;
        const float f = (float)ph / (float)BIT_PERIOD;
        float sz = 0.8f;
        if (f < 0.2f) sz *= f / 0.2f;
        else if (f > 0.75f) sz *= (1.0f - f) / 0.25f;
        if (sz < 0.2f) continue;
        const float y = 9.0f + (4.0f - 9.0f) * f;
        put_unit_rect(canvas, v, BIT_X[i] - sz * 0.5f, y - sz * 0.5f, sz, sz, bdy, BIT_COL);
    }
}

/* --- 完成：四周迸出火花 --- */
#define SPARK_COUNT 6
static const float SPARK_X[SPARK_COUNT] = {-1.6f, 16.6f, 16.2f, -1.2f, 7.5f, -0.8f};
static const float SPARK_Y[SPARK_COUNT] = {5.2f, 4.6f, 11.0f, 12.0f, 3.6f, 8.4f};
static const uint32_t SPARK_DELAY[SPARK_COUNT] = {0, 250, 500, 750, 1000, 580};
#define SPARK_PERIOD 1500

static void props_done(const clawd_canvas_t *canvas, const view_t *v, uint32_t t, float bdy)
{
    for (int i = 0; i < SPARK_COUNT; i++) {
        const uint32_t ph = (t + SPARK_DELAY[i]) % SPARK_PERIOD;
        /* 十字火花分两拍：先亮中心一点，再迸出四条短臂，然后整个消失。
         * 这个两拍结构比"整颗一起闪"生动得多，官方也是这么拆的。 */
        const uint16_t col = (i & 1) ? SPARK_B : SPARK_A;
        const float u = 0.42f;
        if (ph < 220) {
            put_unit_rect(canvas, v, SPARK_X[i] - u * 0.5f, SPARK_Y[i] - u * 0.5f, u, u, bdy, col);
        } else if (ph < 480) {
            put_unit_rect(canvas, v, SPARK_X[i] - u * 0.5f, SPARK_Y[i] - u * 1.9f, u, u, bdy, col);
            put_unit_rect(canvas, v, SPARK_X[i] - u * 0.5f, SPARK_Y[i] + u * 0.9f, u, u, bdy, col);
            put_unit_rect(canvas, v, SPARK_X[i] - u * 1.9f, SPARK_Y[i] - u * 0.5f, u, u, bdy, col);
            put_unit_rect(canvas, v, SPARK_X[i] + u * 0.9f, SPARK_Y[i] - u * 0.5f, u, u, bdy, col);
        }
    }
}

/* --- 等待输入：头顶举起的灯泡 + 一闪一闪的光线 --- */
static void props_waiting(const clawd_canvas_t *canvas, const view_t *v, uint32_t t, float bdy,
                          float lift)
{
    const float ph = (float)(t % 6000) / 6000.0f;
    if (ph < 0.11f || ph > 0.70f) return; /* 灯泡只在举起的那段出现 */

    const bool lit = (ph >= 0.20f);
    const float cx = 14.0f, cy = 5.2f + lift;

    /* 灯泡：玻璃泡 + 灯头，亮起时换色。像素画，不做渐变。 */
    put_unit_rect(canvas, v, cx - 1.1f, cy - 1.1f, 2.2f, 1.9f, bdy,
                  lit ? BULB_ON : BULB_OFF);
    put_unit_rect(canvas, v, cx - 0.55f, cy + 0.8f, 1.1f, 0.7f, bdy,
                  lit ? BULB_EDGE_ON : BULB_EDGE_OFF);

    if (!lit) return;
    /* 光线：按不规则节拍闪。规则闪烁看着像故障灯，不规则才像"灵光一现"。 */
    const uint32_t beat = (t % 1000);
    if (beat > 620) return;
    const float len = 1.0f, w = 0.34f;
    put_unit_rect(canvas, v, cx - w * 0.5f, cy - 2.6f, w, len, bdy, BULB_ON);
    put_unit_rect(canvas, v, cx - 2.6f, cy - w * 0.5f, len, w, bdy, BULB_ON);
    put_unit_rect(canvas, v, cx + 1.6f, cy - w * 0.5f, len, w, bdy, BULB_ON);
    put_unit_rect(canvas, v, cx - 2.1f, cy - 2.1f, 0.7f, 0.7f, bdy, BULB_ON);
    put_unit_rect(canvas, v, cx + 1.5f, cy - 2.1f, 0.7f, 0.7f, bdy, BULB_ON);
}

/* --- 睡觉：飘起来的 Z --- */
static void draw_pixel_z(const clawd_canvas_t *canvas, const view_t *v, float x, float y,
                         float u, float bdy, uint16_t col)
{
    put_unit_rect(canvas, v, x, y, u * 4.0f, u, bdy, col);            /* 上横 */
    put_unit_rect(canvas, v, x + u * 2.0f, y + u, u, u, bdy, col);    /* 斜 */
    put_unit_rect(canvas, v, x + u, y + u * 2.0f, u, u, bdy, col);    /* 斜 */
    put_unit_rect(canvas, v, x, y + u * 3.0f, u * 4.0f, u, bdy, col); /* 下横 */
}

#define Z_COUNT 3
static const uint32_t Z_DELAY[Z_COUNT] = {0, 2000, 4000};
#define Z_PERIOD 6000

static void props_sleeping(const clawd_canvas_t *canvas, const view_t *v, uint32_t t, float bdy)
{
    for (int i = 0; i < Z_COUNT; i++) {
        const uint32_t ph = (t + Z_DELAY[i]) % Z_PERIOD;
        const float f = (float)ph / (float)Z_PERIOD;
        if (f > 0.9f) continue;
        /* 一边飘一边左右摆，越飘越大——直着往上走看着像字幕，摆起来才像气泡 */
        const float sway = sinf(f * 6.28318f * 1.5f) * 1.2f;
        const float x = 11.0f + sway;
        const float y = 9.5f - f * 5.2f;
        const float u = 0.22f + f * 0.20f;
        draw_pixel_z(canvas, v, x, y, u, bdy, ZZZ_COL);
    }
}

/* ------------------------------------------------------------------ *
 * 各状态的动作曲线
 * ------------------------------------------------------------------ */

static void pose_reset(pose_t *p)
{
    memset(p, 0, sizeof(*p));
    p->body_sx = p->body_sy = 1.0f;
    p->eye_scale_y = 1.0f;
    p->shadow_sx = 1.0f;
    p->shadow_alpha = 1.0f;
}

/* 通用：3.2s 呼吸；2s 周期眨眼 */
static void apply_breathe_and_blink(pose_t *p, uint32_t t)
{
    static const key_t BREATHE_X[] = {{0.0f, 1.0f}, {0.5f, 1.02f}, {1.0f, 1.0f}};
    static const key_t BREATHE_Y[] = {{0.0f, 1.0f}, {0.5f, 0.98f}, {1.0f, 1.0f}};
    const float bp = phase_of(t, 3200);
    p->body_sx *= ease_keys(BREATHE_X, 3, bp);
    p->body_sy *= ease_keys(BREATHE_Y, 3, bp);

    static const key_t BLINK[] = {
        {0.00f, 1.0f}, {0.46f, 1.0f}, {0.50f, 0.1f}, {0.54f, 1.0f}, {1.0f, 1.0f}};
    p->eye_scale_y *= ease_keys(BLINK, 5, phase_of(t, 2000));
}

/*
 * 干活中：伏案打字。
 * 左右臂周期 150ms / 120ms 互质——双手看起来不同步，这是让它"像在打字"的关键。
 */
static void pose_working(pose_t *p, uint32_t t)
{
    static const key_t BOUNCE[] = {{0.0f, 0.0f}, {0.5f, 0.8f}, {1.0f, 0.0f}};
    p->body_dy = ease_keys(BOUNCE, 3, phase_of(t, 700)) * 0.06f;

    static const key_t TYPE[] = {
        {0.00f, -5.0f}, {0.25f, -38.0f}, {0.50f, -10.0f}, {0.75f, -30.0f}, {1.00f, -5.0f}};
    /*
     * **周期不能比帧间隔短太多。** 官方是 120/150ms，那是 60fps 浏览器里的数；
     * 这块板子 30fps，120ms 一个周期只采到 3~4 帧，动作直接被采样打碎成抖动——
     * 不是"在打字"，是"在哆嗦"。放慢到 360/300ms，一个周期有 10 帧上下，
     * 敲击的起落才看得出来。两个周期仍然互质，双手依旧不同步。
     */
    p->arm_l_rot = ease_keys(TYPE, 5, phase_of(t, 360)) * (float)M_PI / 180.0f;
    p->arm_r_rot = -ease_keys(TYPE, 5, phase_of(t, 300)) * (float)M_PI / 180.0f;

    static const key_t SHADOW[] = {{0.0f, 1.02f}, {0.5f, 1.05f}, {1.0f, 1.02f}};
    p->shadow_sx = ease_keys(SHADOW, 3, phase_of(t, 400));

    /*
     * **眼睛是眯着的，而且偶尔抬头扫一眼。**
     * 官方 eye-code 是条 10 秒的长曲线：大部分时间 scaleY 0.6 并下移 1 单位
     * （盯着键盘的专注表情），中间穿插几次眨眼，57%~71% 抬起来左右扫两下屏幕。
     * 少了这个，"干活"就只是身体在抖，脸上没有神——不可爱的关键一处。
     */
    const float ep = phase_of(t, 10000);
    static const key_t EYE_SY[] = {
        {0.00f, 0.6f}, {0.14f, 0.6f}, {0.15f, 0.1f}, {0.16f, 0.6f},
        {0.36f, 0.6f}, {0.37f, 0.1f}, {0.38f, 0.6f},
        {0.54f, 0.6f}, {0.55f, 0.1f},
        {0.57f, 1.0f}, {0.69f, 1.0f}, {0.71f, 0.1f}, {0.73f, 0.6f},
        {0.89f, 0.6f}, {0.90f, 0.1f}, {0.91f, 0.6f}, {1.00f, 0.6f}};
    static const key_t EYE_DX[] = {
        {0.00f, 0.0f}, {0.55f, 0.0f}, {0.57f, -1.0f}, {0.62f, 1.5f},
        {0.64f, -0.5f}, {0.69f, 1.5f}, {0.73f, 0.0f}, {1.00f, 0.0f}};
    static const key_t EYE_DY[] = {
        {0.00f, 1.0f}, {0.55f, 1.0f}, {0.57f, -0.5f}, {0.69f, -0.5f},
        {0.73f, 1.0f}, {1.00f, 1.0f}};
    p->eye_scale_y = ease_keys(EYE_SY, 17, ep);
    p->eye_dx = ease_keys(EYE_DX, 8, ep);
    p->eye_dy = ease_keys(EYE_DY, 6, ep);

    /* 呼吸照旧，但眨眼已由上面的曲线接管，不再叠加 */
    static const key_t BX[] = {{0.0f, 1.0f}, {0.5f, 1.02f}, {1.0f, 1.0f}};
    static const key_t BY[] = {{0.0f, 1.0f}, {0.5f, 0.98f}, {1.0f, 1.0f}};
    const float bp = phase_of(t, 3200);
    p->body_sx *= ease_keys(BX, 3, bp);
    p->body_sy *= ease_keys(BY, 3, bp);
}

/*
 * 刚完成：教科书式 squash-and-stretch，1s 循环。
 * 影子与身体**反向耦合**——身体升到最高时影子缩到 0.5、透明度降到 0.15，
 * 这个反相才是"跳得高"的观感来源，比单纯的位移有效得多。
 */
static void pose_done(pose_t *p, uint32_t t)
{
    const float ph = phase_of(t, 1000);

    static const key_t DY[] = {{0.00f, 0.0f},  {0.15f, 0.0f},  {0.20f, 0.0f},
                               {0.40f, -10.0f}, {0.50f, -12.0f}, {0.60f, -10.0f},
                               {0.80f, 0.0f},  {0.85f, 0.0f},  {1.00f, 0.0f}};
    static const key_t SY[] = {{0.00f, 1.0f},  {0.15f, 1.0f},  {0.20f, 0.85f},
                               {0.40f, 1.05f}, {0.50f, 1.0f},  {0.60f, 1.05f},
                               {0.80f, 0.85f}, {0.85f, 1.0f},  {1.00f, 1.0f}};
    /* -12px 是在 30px/单位下标定的，换算成单位量 */
    p->body_dy = ease_keys(DY, 9, ph) / 30.0f;
    p->body_sy = ease_keys(SY, 9, ph);
    p->body_sx = 2.0f - p->body_sy; /* 体积守恒的近似 */

    static const key_t SHADOW_S[] = {{0.00f, 1.0f}, {0.15f, 1.0f}, {0.20f, 1.1f},
                                     {0.40f, 0.6f}, {0.50f, 0.5f}, {0.60f, 0.6f},
                                     {0.80f, 1.1f}, {1.00f, 1.0f}};
    static const key_t SHADOW_A[] = {{0.00f, 0.5f}, {0.15f, 0.5f}, {0.20f, 0.6f},
                                     {0.40f, 0.2f}, {0.50f, 0.15f}, {0.60f, 0.2f},
                                     {0.80f, 0.6f}, {1.00f, 0.5f}};
    p->shadow_sx = ease_keys(SHADOW_S, 8, ph);
    p->shadow_alpha = ease_keys(SHADOW_A, 8, ph) / 0.5f;

    /* 双臂 150ms 交替挥动 45°↔85° */
    static const key_t WAVE[] = {{0.0f, 45.0f}, {0.5f, 85.0f}, {1.0f, 45.0f}};
    const float wave = ease_keys(WAVE, 3, phase_of(t, 320)) * (float)M_PI / 180.0f;
    p->arm_l_rot = -wave;
    p->arm_r_rot = wave;

    p->eye_scale_y = 1.0f;
}

/*
 * 等你输入：把灯泡举过头顶并明显吃力，6s 循环。
 * 颤抖是重点——身体右倾时叠 4 次微抖，腿部在吃力期间抖 6 拍。
 * 这些不规则才让"吃力"读得出来；平滑的抬手只会像在打招呼。
 */
static void pose_waiting(pose_t *p, uint32_t t)
{
    const float ph = phase_of(t, 6000);

    static const key_t TILT[] = {{0.00f, 0.0f}, {0.16f, 0.0f}, {0.24f, 3.0f},
                                 {0.39f, 4.5f}, {0.48f, 2.0f}, {0.57f, 4.0f},
                                 {0.66f, 3.0f}, {0.76f, 0.0f}, {1.00f, 0.0f}};
    p->body_rot = ease_keys(TILT, 9, ph) * (float)M_PI / 180.0f;

    static const key_t SQUASH[] = {{0.00f, 1.0f}, {0.16f, 1.0f}, {0.24f, 0.97f},
                                   {0.66f, 0.97f}, {0.76f, 1.0f}, {1.00f, 1.0f}};
    p->body_sy = ease_keys(SQUASH, 6, ph);

    /* 右臂抬起 → 吃力 → 保持约 2.5s → 放下 */
    static const key_t ARM_UP[] = {{0.00f, 0.0f}, {0.03f, 0.0f}, {0.10f, -3.0f},
                                   {0.16f, -3.0f}, {0.24f, -4.5f}, {0.66f, -4.5f},
                                   {0.76f, -3.0f}, {0.80f, 0.0f}, {1.00f, 0.0f}};
    static const key_t ARM_ROT[] = {{0.00f, 0.0f}, {0.10f, -90.0f}, {0.66f, -90.0f},
                                    {0.80f, 0.0f}, {1.00f, 0.0f}};
    p->arm_r_dy = ease_keys(ARM_UP, 9, ph) / 15.0f;
    p->arm_r_rot = ease_keys(ARM_ROT, 5, ph) * (float)M_PI / 180.0f;

    /* 吃力期间腿部 6 拍颤抖 */
    static const key_t TREMOR[] = {{0.00f, 0.0f}, {0.16f, 0.0f}, {0.24f, 22.0f},
                                   {0.36f, 26.0f}, {0.42f, 19.0f}, {0.48f, 26.0f},
                                   {0.54f, 19.0f}, {0.60f, 26.0f}, {0.66f, 22.0f},
                                   {0.76f, 0.0f}, {1.00f, 0.0f}};
    const float tremor = ease_keys(TREMOR, 11, ph) * (float)M_PI / 180.0f;
    p->leg_rot[0] = -tremor * 0.5f;
    p->leg_rot[1] = -tremor * 0.2f;
    p->leg_rot[2] = tremor * 0.2f;
    p->leg_rot[3] = tremor * 0.5f;

    /* 重量离地：影子收缩变淡 */
    static const key_t SHADOW[] = {{0.00f, 1.0f}, {0.24f, 0.86f}, {0.66f, 0.86f},
                                   {0.80f, 1.0f}, {1.00f, 1.0f}};
    p->shadow_sx = ease_keys(SHADOW, 5, ph);
    p->shadow_alpha = p->shadow_sx;

    /*
     * 眼睛：先抬头看灯泡（16%~24%），30% 眨一下，然后**切成 ^^ 笑眼**
     * 一直保持到 72%，最后再睁回方块眼。
     *
     * 那个 ^^ 才是这个动作的表情核心——"我想到了，快来看"。
     * 我原来只做了眯眼，结果整段动作只剩"吃力"没有"高兴"，
     * 一个举着灯泡皱眉的家伙当然不可爱。
     */
    static const key_t EYE_DX[] = {{0.00f, 0.0f}, {0.11f, 0.0f}, {0.16f, 1.0f},
                                   {0.24f, 1.0f}, {0.28f, 0.0f}, {1.00f, 0.0f}};
    static const key_t EYE_DY[] = {{0.00f, 0.0f}, {0.11f, 0.0f}, {0.16f, -0.5f},
                                   {0.24f, -0.5f}, {0.28f, 0.0f}, {1.00f, 0.0f}};
    p->eye_dx = ease_keys(EYE_DX, 6, ph);
    p->eye_dy = ease_keys(EYE_DY, 6, ph);

    static const key_t BLINK[] = {{0.00f, 1.0f}, {0.29f, 1.0f}, {0.30f, 0.1f},
                                  {0.72f, 0.1f}, {0.74f, 1.0f}, {1.00f, 1.0f}};
    p->eye_scale_y = ease_keys(BLINK, 6, ph);
    /* 31%~72% 换成笑眼；step 切换，不做插值 */
    p->eye_mode = (ph >= 0.31f && ph < 0.72f) ? 1 : 0;
    if (p->eye_mode == 1) p->eye_scale_y = 1.0f;
}

/*
 * 睡觉：摊成一滩 + 深呼吸。
 *
 * 呼吸绕**脚底**做，而且纵向幅度极大（1.25 倍）——胸腔明显鼓起来又落下去。
 * 站姿呼吸只有 2%，因为那是"活着"；睡姿要 25%，因为那是"睡熟了"。
 * 数值来自官方 clawd-sleeping.svg 的 breathe-squash，4.5s 一轮。
 */
static void pose_sleeping(pose_t *p, uint32_t t)
{
    p->body_form = 2;
    p->eye_mode = 2;

    static const key_t SQX[] = {{0.0f, 1.0f}, {0.30f, 1.02f}, {0.40f, 1.02f},
                                {0.80f, 1.0f}, {1.0f, 1.0f}};
    static const key_t SQY[] = {{0.0f, 1.0f}, {0.30f, 1.25f}, {0.40f, 1.25f},
                                {0.80f, 1.0f}, {1.0f, 1.0f}};
    const float ph = phase_of(t, 4500);
    p->body_sx = ease_keys(SQX, 5, ph);
    p->body_sy = ease_keys(SQY, 5, ph);

    static const key_t SH[] = {{0.0f, 1.0f}, {0.30f, 1.05f}, {0.40f, 1.05f},
                               {0.80f, 1.0f}, {1.0f, 1.0f}};
    p->shadow_sx = ease_keys(SH, 5, ph);
}

/* ------------------------------------------------------------------ *
 * 对外接口
 * ------------------------------------------------------------------ */

uint32_t clawd_cycle_ms(clawd_state_t state)
{
    switch (state) {
        case CLAWD_WORKING: return 3200;
        case CLAWD_DONE: return 1000;
        case CLAWD_WAITING: return 6000;
        case CLAWD_SLEEPING: return 5000;
        case CLAWD_IDLE:
        default: return 3200;
    }
}

bool clawd_done_finished(uint32_t elapsed_ms) { return elapsed_ms >= 1200; }

clawd_rect_t clawd_bounds(const clawd_draw_t *p)
{
    /*
     * 只按**实际占用**的单位范围算，不要用整个 15×16 viewBox——
     * 人物只占 y=6..15 这 9 个单位，上面 6 个单位是空的，
     * 按整格算会白清掉将近一半屏幕，帧耗时直接翻倍。
     *
     * 余量 2 单位覆盖：手臂旋转外甩、DONE 态跳跃、等待态举臂、重力位移。
     */
    const float margin = 2.0f;
    const float s = p->px_per_unit;
    const float top_unit = 6.0f;   /* 头顶 */
    const float bot_unit = 15.0f;  /* 脚底 = BODY_PIVOT_Y */

    const float left = (float)p->center_x - (BODY_PIVOT_X + margin) * s + p->tilt_x;
    const float top =
        (float)p->baseline_y - (bot_unit - top_unit + margin) * s + p->tilt_y;
    const float w = ((float)CLAWD_UNIT_W + margin * 2.0f) * s;
    const float h = (bot_unit - top_unit + margin * 2.0f) * s;

    clawd_rect_t r = {(int)floorf(left), (int)floorf(top), (int)ceilf(w), (int)ceilf(h)};
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

void clawd_draw(const clawd_canvas_t *canvas, const clawd_draw_t *p)
{
    if (canvas == NULL || canvas->pixels == NULL || p == NULL) return;

    pose_t pose;
    pose_reset(&pose);
    switch (p->state) {
        case CLAWD_WORKING: pose_working(&pose, p->elapsed_ms); break;
        case CLAWD_DONE: pose_done(&pose, p->elapsed_ms); break;
        case CLAWD_WAITING: pose_waiting(&pose, p->elapsed_ms); break;
        case CLAWD_SLEEPING: pose_sleeping(&pose, p->elapsed_ms); break;
        case CLAWD_IDLE:
        default:
            /* 站着发呆用**加长的腿**，身体立起来才有精神——官方 idle 就是这么画的 */
            pose.body_form = 1;
            apply_breathe_and_blink(&pose, p->elapsed_ms);
            break;
    }

    /* 单位原点：让 BODY_PIVOT 落在 (center_x, baseline_y)，再叠加重力位移 */
    const view_t view = {
        .ox = (float)p->center_x - BODY_PIVOT_X * p->px_per_unit + p->tilt_x,
        .oy = (float)p->baseline_y - BODY_PIVOT_Y * p->px_per_unit + p->tilt_y,
        .scale = p->px_per_unit,
    };

    pt_t quad[4];

    /*
     * 不画地面影子。
     * 试过用比背景更暗的颜色画一条，实际观感就是精灵脚下一条突兀的黑杠——
     * 在纯色深背景上，"比背景更暗"读不出"影子"，只读出"一根线"。
     * 跳跃的高度感靠 squash-and-stretch 本身已经足够。
     * pose.shadow_* 仍在计算，留给日后改成柔和椭圆或渐变时直接用。
     */
    const float bdx = 0.0f;
    const float bdy = pose.body_dy;

#define BODY_XFORM(rect_ptr)                                                            \
    transform_rect(&view, (rect_ptr), BODY_PIVOT_X, BODY_PIVOT_Y, pose.body_rot,        \
                   pose.body_sx, pose.body_sy, bdx, bdy, quad)

    /*
     * 按姿势挑几何。腿在不同状态下长短不同，睡姿更是整套换掉——
     * 官方每个状态的 SVG 都是重画的，不是同一套图形做变换。
     */
    const rect_t *legs = pose.body_form == 2   ? R_SLEEP_LEGS
                         : pose.body_form == 1 ? R_LEGS_TALL
                                               : R_LEGS;
    const rect_t *torso = pose.body_form == 2 ? &R_SLEEP_TORSO : &R_TORSO;
    const rect_t *arm_l = pose.body_form == 2 ? &R_SLEEP_ARM_L : &R_ARM_L;
    const rect_t *arm_r = pose.body_form == 2 ? &R_SLEEP_ARM_R : &R_ARM_R;

    /* 腿（各自带一点颤抖旋转，支点在腿根） */
    for (int i = 0; i < 4; i++) {
        const rect_t *leg = &legs[i];
        transform_rect(&view, leg, leg->x + leg->w * 0.5f, leg->y, pose.leg_rot[i],
                       pose.body_form == 2 ? pose.body_sx : 1.0f,
                       pose.body_form == 2 ? pose.body_sy : 1.0f, bdx, bdy, quad);
        fill_quad(canvas, quad, p->body_color);
    }

    /* 躯干 */
    BODY_XFORM(torso);
    fill_quad(canvas, quad, p->body_color);

    /* 双臂 */
    if (pose.body_form == 2) {
        /* 睡姿的手是摊在地上的，跟着身体一起缩放，不单独旋转 */
        BODY_XFORM(arm_l);
        fill_quad(canvas, quad, p->body_color);
        BODY_XFORM(arm_r);
        fill_quad(canvas, quad, p->body_color);
    } else {
        transform_rect(&view, arm_l, ARM_L_PIVOT_X, ARM_L_PIVOT_Y, pose.arm_l_rot, 1.0f,
                       1.0f, bdx, bdy, quad);
        fill_quad(canvas, quad, p->body_color);
        transform_rect(&view, arm_r, ARM_R_PIVOT_X, ARM_R_PIVOT_Y, pose.arm_r_rot, 1.0f,
                       1.0f, bdx, bdy + pose.arm_r_dy, quad);
        fill_quad(canvas, quad, p->body_color);
    }

    /*
     * 眼睛。三种形态：
     *   0 方块眼（纵向压缩即眨眼）
     *   1 ^^ 笑眼——两撇朝上的折线，用两个斜矩形拼
     *   2 睡觉的细线
     * 表情是"可爱"的绝大部分，只做眨眼是不够的。
     */
    if (pose.eye_mode == 1) {
        /* ^^ ：每只眼用两小段拼成尖朝上的折线，端点取自官方 polyline */
        static const float SMILE[2][4] = {{3.5f, 4.5f, 9.5f, 10.5f}, {4.5f, 5.5f, 10.5f, 11.5f}};
        for (int e = 0; e < 2; e++) {
            const float cx = (e == 0) ? 4.5f : 10.5f;
            for (int half = 0; half < 2; half++) {
                const float x0 = (half == 0) ? cx - 1.0f : cx;
                rect_t seg = {x0, 8.6f, 1.0f, 0.45f};
                const float rot = (half == 0 ? -40.0f : 40.0f) * (float)M_PI / 180.0f;
                transform_rect(&view, &seg, half == 0 ? x0 + 1.0f : x0, seg.y + seg.h * 0.5f,
                               rot, pose.body_sx, pose.body_sy, bdx, bdy, quad);
                fill_quad(canvas, quad, p->eye_color);
            }
        }
        (void)SMILE;
    } else {
        const float eye_scale = pose.eye_scale_y < 0.05f ? 0.05f : pose.eye_scale_y;
        rect_t el = pose.eye_mode == 2 ? R_SLEEP_EYE_L : R_EYE_L;
        rect_t er = pose.eye_mode == 2 ? R_SLEEP_EYE_R : R_EYE_R;
        const float shrink_l = el.h * (1.0f - eye_scale) * 0.5f;
        el.y += shrink_l;
        el.h *= eye_scale;
        er.y += shrink_l;
        er.h *= eye_scale;
        el.x += pose.eye_dx;
        er.x += pose.eye_dx;
        el.y += pose.eye_dy;
        er.y += pose.eye_dy;

        transform_rect(&view, &el, BODY_PIVOT_X, BODY_PIVOT_Y, pose.body_rot, pose.body_sx,
                       pose.body_sy, bdx, bdy, quad);
        fill_quad(canvas, quad, p->eye_color);
        transform_rect(&view, &er, BODY_PIVOT_X, BODY_PIVOT_Y, pose.body_rot, pose.body_sx,
                       pose.body_sy, bdx, bdy, quad);
        fill_quad(canvas, quad, p->eye_color);
    }

    /* 道具画在最后：它们都在人物轮廓之外，不会被身体盖住 */
    switch (p->state) {
        case CLAWD_WORKING: props_working(canvas, &view, p->elapsed_ms, bdy); break;
        case CLAWD_DONE: props_done(canvas, &view, p->elapsed_ms, bdy); break;
        case CLAWD_WAITING: props_waiting(canvas, &view, p->elapsed_ms, bdy, pose.arm_r_dy); break;
        case CLAWD_SLEEPING: props_sleeping(canvas, &view, p->elapsed_ms, bdy); break;
        default: break;
    }

#undef BODY_XFORM
}
