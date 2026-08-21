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

            /*
             * 横向精确覆盖率：**只有两端要算，中间整格恒等于 1。**
             *
             * 原来对区间里每个像素都调一次 span_cov，而且每行要调三遍
             * （三条子扫描线）。一个 164 像素宽的躯干就是每行 492 次函数调用，
             * 其中 480 多次的答案都是 1。拆成"左边缘 / 中间 / 右边缘"三段之后，
             * 中间那段退化成一个加法——这是通用路径最大的一块浪费。
             */
            int xa = (int)floorf(lo), xb = (int)ceilf(hi);
            if (xa < x0) xa = x0;
            if (xb > x1) xb = x1;
            int m0 = (int)ceilf(lo);
            int m1 = (int)floorf(hi);
            if (m0 < xa) m0 = xa;
            if (m0 > xb) m0 = xb;
            if (m1 < m0) m1 = m0;
            if (m1 > xb) m1 = xb;
            for (int x = xa; x < m0; x++) s_cov[x - x0] += span_cov(lo, hi, x) * share;
            for (int x = m0; x < m1; x++) s_cov[x - x0] += share;
            for (int x = m1; x < xb; x++) s_cov[x - x0] += span_cov(lo, hi, x) * share;
        }
        if (!any) continue;

        uint16_t *row = c->pixels + (size_t)y * c->width;
        for (int x = x0; x < x1; x++) {
            const float a = s_cov[x - x0];
            if (a <= 0.004f) continue;
            /*
             * **完全覆盖的内部像素直接写，不走混色。**
             * 一个图形里绝大多数像素都是满覆盖的，只有边缘那一圈需要混色；
             * 对每个像素都做一次 blend565（拆通道 → 乘 → 拼回去）纯属浪费。
             * 躯干带一点点旋转就会掉进这条通用路径，它一个人就占三成帧时间——
             * 加这一行之后，旋转图形的代价和轴对齐图形基本拉平了。
             */
            if (a > 0.996f) { row[x] = color; continue; }
            row[x] = blend565(row[x], color, a);
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
    /* 节拍驱动量：敲击强度、捂耳压下的量。**必须由同一个 beat 算出来**，
     * 道具才能和动作对上——各自单独计时就会各闪各的。 */
    float tap;
    float ear_press;
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
/* 灭掉的灯泡是**冷灰玻璃**，不是暗黄。用 0x8A7428 那档暗黄时，
 * 屏上读出来是一坨脏橄榄绿，像发霉不像玻璃。 */
#define BULB_OFF clawd_rgb565(0x70, 0x79, 0x86)
#define BULB_EDGE_ON clawd_rgb565(0xFF, 0xB0, 0x00)
#define BULB_EDGE_OFF clawd_rgb565(0x45, 0x4B, 0x54)
#define ZZZ_COL clawd_rgb565(0xB0, 0xBE, 0xC5)

/** 抗锯齿的圆。泡泡、火花这类小圆件用它，方块拼出来的圆在这个尺度上很硬。 */
static void put_unit_circle(const clawd_canvas_t *canvas, const view_t *v, float cx, float cy,
                            float r, float bdy, uint16_t color)
{
    const pt_t c = to_px(v, cx, cy + bdy);
    const float pr = r * v->scale;
    if (pr <= 0.3f) return;
    int y0 = (int)floorf(c.y - pr), y1 = (int)ceilf(c.y + pr);
    int x0 = (int)floorf(c.x - pr), x1 = (int)ceilf(c.x + pr);
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > canvas->height) y1 = canvas->height;
    if (x1 > canvas->width) x1 = canvas->width;

    for (int y = y0; y < y1; y++) {
        uint16_t *row = canvas->pixels + (size_t)y * canvas->width;
        for (int x = x0; x < x1; x++) {
            const float dx = (float)x + 0.5f - c.x;
            const float dy = (float)y + 0.5f - c.y;
            const float d = sqrtf(dx * dx + dy * dy);
            /* 边缘一个像素内做线性过渡，就足够消掉锯齿 */
            const float a = pr - d + 0.5f;
            if (a <= 0.0f) continue;
            row[x] = blend565(row[x], color, a > 1.0f ? 1.0f : a);
        }
    }
}

/** 抗锯齿椭圆。台面上的唱片是斜着看的圆——画成正圆就成了立着的轮子。 */
static void put_unit_ellipse(const clawd_canvas_t *canvas, const view_t *v, float cx,
                             float cy, float rx, float ry, float bdy, uint16_t color)
{
    const pt_t c = to_px(v, cx, cy + bdy);
    const float px = rx * v->scale, py = ry * v->scale;
    if (px <= 0.3f || py <= 0.3f) return;
    int y0 = (int)floorf(c.y - py), y1 = (int)ceilf(c.y + py);
    int x0 = (int)floorf(c.x - px), x1 = (int)ceilf(c.x + px);
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 > canvas->height) y1 = canvas->height;
    if (x1 > canvas->width) x1 = canvas->width;
    /* 用归一化距离的梯度做边缘过渡，等价于圆的"半径差"，但对扁椭圆同样成立 */
    for (int y = y0; y < y1; y++) {
        uint16_t *row = canvas->pixels + (size_t)y * canvas->width;
        for (int x = x0; x < x1; x++) {
            const float dx = ((float)x + 0.5f - c.x) / px;
            const float dy = ((float)y + 0.5f - c.y) / py;
            const float d = sqrtf(dx * dx + dy * dy);
            if (d >= 1.3f) continue;
            /* 把归一化梯度换算回像素，边缘一像素内线性过渡 */
            const float grad = (px < py ? px : py);
            const float a = (1.0f - d) * grad + 0.5f;
            if (a <= 0.0f) continue;
            row[x] = blend565(row[x], color, a > 1.0f ? 1.0f : a);
        }
    }
}

static void put_unit_rect(const clawd_canvas_t *canvas, const view_t *v, float x, float y,
                          float w, float h, float bdy, uint16_t color)
{
    pt_t quad[4];
    const rect_t r = {x, y, w, h};
    transform_rect(v, &r, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, bdy, quad);
    fill_quad(canvas, quad, color);
}

/** 任意四边形（单位坐标）。梯形要靠它——transform_rect 只能给出平行四边形。 */
static void put_unit_quad(const clawd_canvas_t *canvas, const view_t *v, const float xs[4],
                          const float ys[4], float bdy, uint16_t color)
{
    pt_t q[4];
    for (int i = 0; i < 4; i++) q[i] = to_px(v, xs[i], ys[i] + bdy);
    fill_quad(canvas, q, color);
}

/* ------------------------------------------------------------------ *
 * 干活：打碟的 DJ
 *
 * **所有东西挂在同一个节拍上**——点头、搓碟、扣耳麦、灯光脉冲、
 * 唱片被搓时的顿挫，全部由同一个 beat 驱动。此前每个动作各有各的周期，
 * 看起来就是几个零件各抖各的；"动作假"的根子在这儿，不在曲线精度。
 * ------------------------------------------------------------------ */

#define BEAT_MS 520

/*
 * 耳麦。上一版只有"一条弧 + 两个圆片"，缺了真正决定辨识度的那件东西：
 * **叉臂**——从头梁伸下来夹住耳罩的那两根короткие短杆。没有它，
 * 罩子就是贴在脸侧的两个圆，怎么调都读不出"戴在头上"。
 *
 * 另外 DJ 耳麦是大罩：竖着比横着长，而且厚。做成小圆片就是通勤耳机。
 */
#define HP_MID clawd_rgb565(0x5A, 0x62, 0x6B)
#define HP_DARK clawd_rgb565(0x24, 0x28, 0x2D)
#define HP_PAD clawd_rgb565(0x33, 0x38, 0x3E)
#define HP_HI clawd_rgb565(0xA2, 0xAD, 0xB8)

/* 唱机 */
#define DECK_TOP clawd_rgb565(0x2B, 0x30, 0x36)
#define DECK_EDGE clawd_rgb565(0x14, 0x16, 0x19)
#define VINYL clawd_rgb565(0x15, 0x17, 0x1A)
#define VINYL_GROOVE clawd_rgb565(0x33, 0x39, 0x40)
#define VINYL_MARK clawd_rgb565(0xEC, 0xF0, 0xF4)

/* 夜店灯：三种冷暖对冲的颜色轮着来，才有"打灯"的感觉；
 * 一色到底只会像蒙了张色纸。 */
#define LIGHT_A clawd_rgb565(0xFF, 0x3F, 0xA4)
#define LIGHT_B clawd_rgb565(0x38, 0xE8, 0xFF)
#define LIGHT_C clawd_rgb565(0x9B, 0x5D, 0xE5)
#define LIGHT_D clawd_rgb565(0xFF, 0xB0, 0x20)

/** 取同色系的暗一档。手臂摆到身前时与躯干同色会整条消失——
 *  官方那套图形手臂本来就甩在体侧，不存在这个问题；一旦让手去够东西，
 *  就必须有明暗把前后分开。压得很轻（0.86），远看仍是一整块。 */
static uint16_t shade565(uint16_t c, float k)
{
    const int r = (int)(((c >> 11) & 0x1F) * k);
    const int g = (int)(((c >> 5) & 0x3F) * k);
    const int b = (int)((c & 0x1F) * k);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/** 沿圆弧铺一串小四边形。头梁这种"细弯条"用它——
 *  拿几个矩形硬拼，弯折处会露出台阶。相邻两段共边，所以不会有缝。 */
static void put_arc(const clawd_canvas_t *canvas, const view_t *v, float cx, float cy,
                    float r, float a0, float a1, float thick, float bdy, uint16_t color)
{
    const int seg = 14;
    const float half = thick * 0.5f;
    /* 相邻两段**故意重叠一点点**。严丝合缝地拼时，两条抗锯齿边各自只覆盖
     * 半个像素，加起来仍然不满——沿着弧就会出现一串缺口，
     * 在高光那种细条上尤其明显，看着像虚线。 */
    const float ov = (a1 - a0) * 0.02f;
    for (int i = 0; i < seg; i++) {
        const float t0 = a0 + (a1 - a0) * (float)i / (float)seg - (i ? ov : 0.0f);
        const float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)seg;
        const float xs[4] = {cx + cosf(t0) * (r - half), cx + cosf(t1) * (r - half),
                             cx + cosf(t1) * (r + half), cx + cosf(t0) * (r + half)};
        const float ys[4] = {cy + sinf(t0) * (r - half), cy + sinf(t1) * (r - half),
                             cy + sinf(t1) * (r + half), cy + sinf(t0) * (r + half)};
        put_unit_quad(canvas, v, xs, ys, bdy, color);
    }
}

/*
 * 头梁那条弧是解出来的：要求同时过左罩顶 (1.95, 6.50)、右罩顶 (13.05, 6.50)
 * 和头顶 (7.5, 4.50)，得到圆心 (7.5, 13.20)、半径 8.70。
 */
#define HP_CUP_L_X 1.95f
#define HP_CUP_R_X 13.05f
/* 罩心压在 9.0：脑袋是 6~13，中线在 9.5。原来放 8.2，整副耳麦明显偏上，
 * 看着像架在额头上而不是扣在耳朵上。 */
#define HP_CUP_Y 9.00f
#define HP_CUP_RX 1.52f
#define HP_CUP_RY 1.88f
#define HP_ARC_CX 7.50f
#define HP_ARC_CY 12.745f
#define HP_ARC_R 7.845f

static void props_headphones(const clawd_canvas_t *canvas, const view_t *v, float bdy,
                             float press)
{
    /* 1) 叉臂先画，等下被耳罩压住上半截——这个遮挡关系正是"夹住"的观感来源 */
    for (int i = 0; i < 2; i++) {
        const float cx = i ? HP_CUP_R_X : HP_CUP_L_X;
        put_unit_rect(canvas, v, cx - 0.30f, 6.70f, 0.60f, 2.50f, bdy, HP_DARK);
        put_unit_rect(canvas, v, cx - 0.30f, 6.70f, 0.16f, 2.50f, bdy, HP_HI);
    }

    /* 2) 头梁。厚一点才像 DJ 耳麦，细了就是通勤耳机。 */
    put_arc(canvas, v, HP_ARC_CX, HP_ARC_CY, HP_ARC_R, 3.927f, 5.498f, 0.62f, bdy, HP_MID);
    put_arc(canvas, v, HP_ARC_CX, HP_ARC_CY, HP_ARC_R + 0.24f, 3.99f, 5.44f, 0.15f, bdy,
            HP_HI);

    /* 3) 耳罩：竖椭圆的大罩 + 内圈耳垫。press 是手往里压的量，
     *    罩子跟着略微被压扁——手压下去而罩子纹丝不动，那只手就只是贴在旁边。 */
    for (int i = 0; i < 2; i++) {
        const float cx = i ? HP_CUP_R_X : HP_CUP_L_X;
        const float sq = (i == 0) ? press * 0.09f : 0.0f;
        /* 先画一圈亮的，再把主体略偏右下压上去 —— 左上留出一道月牙高光。
         * 同心圆套同心圆读出来是照相机镜头，有了偏心的高光才是个球面罩子。 */
        put_unit_ellipse(canvas, v, cx - 0.10f, HP_CUP_Y - 0.12f, HP_CUP_RX - sq + 0.05f,
                         HP_CUP_RY - sq * 0.5f + 0.05f, bdy, HP_HI);
        put_unit_ellipse(canvas, v, cx, HP_CUP_Y, HP_CUP_RX - sq, HP_CUP_RY - sq * 0.5f,
                         bdy, HP_MID);
        put_unit_ellipse(canvas, v, cx, HP_CUP_Y, HP_CUP_RX * 0.70f, HP_CUP_RY * 0.70f,
                         bdy, HP_PAD);
        put_unit_ellipse(canvas, v, cx, HP_CUP_Y, HP_CUP_RX * 0.44f, HP_CUP_RY * 0.44f,
                         bdy, HP_DARK);
    }
}

/*
 * DJ 台。
 *
 * **必须先有台面这个"平面"，碟才躺得住。** 上一版只画了一条细边，
 * 碟就成了两个立在那儿的轮子——不是椭圆画得不够扁，是眼睛没有参照面，
 * 于是把椭圆读成了正面看的圆。画出一块朝远处收进去的梯形台面，
 * 同样的椭圆立刻就"躺"下去了。
 *
 * 台子要大。DJ 台本来就是横在人前的一整张控制台，
 * 做成腰间一条小板子既不像，也撑不起那两张碟。
 * 台面盖住腿是对的——真实的 DJ 就是只露上半身站在台后。
 */
/* 远沿从 11.50 下移到 11.85，正面板从 1.40 削到 0.75：
 * 台子整体变矮，身体多露出来一截。
 *
 * 能下移多少是被**手臂够得到的深度**卡死的：右臂绕肩 (13,10) 转、
 * 臂长 2，指尖最低只到 y≈12.24。碟面顶边必须高于远沿、又要低到
 * 手压得住，两头一夹就只剩这 0.35 个单位的余量。 */
#define DECK_FAR_Y 11.85f
#define DECK_NEAR_Y 13.90f
/* 合成框只到 x=-2..17（clawd_bounds 的 margin=2）。原来台子画到 -3.5..18.5，
 * 两端直接被框切平——屏幕上是一条竖直的断口。台面可以比角色宽，
 * 但不能比框宽。 */
#define DECK_FAR_X0 1.70f
#define DECK_FAR_X1 13.30f
#define DECK_NEAR_X0 (-1.85f)
#define DECK_NEAR_X1 16.85f
#define DISC_L_X 4.20f
#define DISC_R_X 11.60f
#define DISC_Y 12.78f
/* 半径比 2.5 : 1.15 ≈ 2.2:1，正好等于台面自身的透视压缩比——
 * 碟和台子的"倾角"必须一致，差一点就会显得碟是斜插在台面上的。 */
#define DISC_RX 2.40f
#define DISC_RY 0.88f

/** 一张唱片：碟体 + 一圈纹路 + 标签 + 一个转动的白点。
 *  那个白点是**唯一能读出"它在转"**的东西——没有它，碟就是个静止的椭圆。 */
static void draw_disc(const clawd_canvas_t *canvas, const view_t *v, float cx, float phi,
                      uint16_t label)
{
    put_unit_ellipse(canvas, v, cx, DISC_Y, DISC_RX, DISC_RY, 0.0f, VINYL);
    put_unit_ellipse(canvas, v, cx, DISC_Y, DISC_RX * 0.78f, DISC_RY * 0.78f, 0.0f,
                     VINYL_GROOVE);
    put_unit_ellipse(canvas, v, cx, DISC_Y, DISC_RX * 0.68f, DISC_RY * 0.68f, 0.0f, VINYL);
    put_unit_ellipse(canvas, v, cx, DISC_Y, DISC_RX * 0.30f, DISC_RY * 0.30f, 0.0f, label);
    put_unit_ellipse(canvas, v, cx + cosf(phi) * DISC_RX * 0.46f,
                     DISC_Y + sinf(phi) * DISC_RY * 0.46f, 0.17f, 0.09f, 0.0f, VINYL_MARK);
}

static void props_decks(const clawd_canvas_t *canvas, const view_t *v, uint32_t t,
                        float scratch, uint16_t label)
{
    /* 整段不吃 bdy：台子立在地上，不跟着身体点头。
     * 让它一起弹的话，"人在动"就变成"人和台子一起在动"，节奏感全丢。 */

    /* 台面：近边宽、远边窄，朝远处收进去。这块梯形就是全部"平面感"的来源。 */
    put_unit_quad(canvas, v,
                  (const float[]){DECK_FAR_X0, DECK_FAR_X1, DECK_NEAR_X1, DECK_NEAR_X0},
                  (const float[]){DECK_FAR_Y, DECK_FAR_Y, DECK_NEAR_Y, DECK_NEAR_Y}, 0.0f,
                  DECK_TOP);
    /* 远沿一条亮线：台面的后缘。没有它，台面和背后的黑融成一片。 */
    put_unit_quad(canvas, v,
                  (const float[]){DECK_FAR_X0, DECK_FAR_X1, DECK_FAR_X1 - 0.10f,
                                  DECK_FAR_X0 + 0.10f},
                  (const float[]){DECK_FAR_Y, DECK_FAR_Y, DECK_FAR_Y + 0.16f,
                                  DECK_FAR_Y + 0.16f},
                  0.0f, HP_MID);
    /* 台子正面 */
    put_unit_quad(canvas, v,
                  (const float[]){DECK_NEAR_X0, DECK_NEAR_X1, DECK_NEAR_X1, DECK_NEAR_X0},
                  (const float[]){DECK_NEAR_Y, DECK_NEAR_Y, DECK_NEAR_Y + 1.10f,
                                  DECK_NEAR_Y + 1.10f},
                  0.0f, DECK_EDGE);

    /* 两张碟 */
    const float base = (float)t * 6.2832f / 900.0f;
    draw_disc(canvas, v, DISC_L_X, base, label);
    /* 右碟正被手搓，转角被拽回去——**顿挫来自手，不是碟自己在抖** */
    draw_disc(canvas, v, DISC_R_X, base - scratch * 3.4f, label);

    /* 中间的调音台。**要满。** 空荡荡的台面读出来是块塑料板，
     * 真正让它像器材的是密密麻麻的旋钮、推子和跑灯。 */
    put_unit_quad(canvas, v, (const float[]){6.05f, 9.55f, 10.05f, 5.55f},
                  (const float[]){11.75f, 11.75f, 13.75f, 13.75f}, 0.0f, DECK_EDGE);

    /* 两排 EQ 旋钮，各带一根指示线；角度各不相同才像是被人调过的 */
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 4; c++) {
            const float kx = 6.35f + (float)c * 0.78f + (float)r * 0.10f;
            const float ky = 12.05f + (float)r * 0.52f;
            put_unit_ellipse(canvas, v, kx, ky, 0.27f, 0.15f, 0.0f, DECK_TOP);
            put_unit_ellipse(canvas, v, kx, ky, 0.17f, 0.09f, 0.0f, DECK_EDGE);
            const float ka = 2.1f + (float)(r * 4 + c) * 0.62f;
            put_unit_rect(canvas, v, kx + cosf(ka) * 0.14f, ky + sinf(ka) * 0.08f, 0.09f,
                          0.07f, 0.0f, VINYL_MARK);
        }
    }
    /*
     * 交叉推子：**横着放，左右扫。**
     *
     * 上一版做成三根竖槽、滑块上下走——台面是朝远处铺开的平面，
     * 竖槽画成轴对齐矩形等于把它贴在了一堵墙上，和当初碟片看着像
     * "立着的轮子"是同一个毛病：物件没有跟随所在平面的透视。
     * 横线在透视平面上本来就不变形，所以横推子不需要额外校正就成立。
     *
     * 而且它跟着**搓碟的同一个量**走。真实的搓碟就是一手推碟、
     * 一手甩交叉推子（crab/transformer），两只手同源才不像各动各的。
     */
    const float xf_x0 = 6.30f, xf_w = 3.10f, xf_y = 13.24f;
    put_unit_rect(canvas, v, xf_x0, xf_y, xf_w, 0.24f, 0.0f, DECK_EDGE);
    put_unit_rect(canvas, v, xf_x0, xf_y + 0.08f, xf_w, 0.08f, 0.0f, DECK_TOP);
    float xf = 0.5f + scratch * 0.45f;
    if (xf < 0.0f) xf = 0.0f;
    if (xf > 1.0f) xf = 1.0f;
    const float knob_w = 0.46f;
    put_unit_rect(canvas, v, xf_x0 + xf * (xf_w - knob_w), xf_y - 0.16f, knob_w, 0.56f,
                  0.0f, HP_MID);
    put_unit_rect(canvas, v, xf_x0 + xf * (xf_w - knob_w) + 0.16f, xf_y - 0.16f, 0.10f,
                  0.56f, 0.0f, HP_HI);
    /* 电平灯跟着拍子跑 */
    const int level = 1 + (int)(scratch * 5.0f);
    for (int i = 0; i < 6; i++) {
        const uint16_t col = i < level ? (i > 3 ? LIGHT_A : LIGHT_B) : DECK_TOP;
        put_unit_rect(canvas, v, 8.15f + (float)i * 0.30f, 13.10f, 0.20f, 0.20f, 0.0f, col);
    }
    /* 两侧各一条变速滑轨 + 四个彩色触发键 —— DJ 台的标志性零件 */
    for (int sd = 0; sd < 2; sd++) {
        const float bx = sd ? 14.30f : 0.55f;
        put_unit_rect(canvas, v, bx, 12.30f, 0.16f, 1.05f, 0.0f, DECK_EDGE);
        put_unit_rect(canvas, v, bx - 0.16f, 12.30f + (sd ? 0.55f : 0.30f), 0.48f, 0.18f,
                      0.0f, HP_MID);

    }
}

/* --- 干活：前景的 bling --- */

#define BLING_COUNT 10
/*
 * 星光只在**角色周围**闪，不落到台面上。
 * 台子是块实心器材，星星落在上面读出来是屏幕坏点；
 * 围着人物闪才是"打在他身上的光"。所以 y 全部压在 11 以上（台面之上）。
 */
static const float BLING_X[BLING_COUNT] = {-1.3f, 16.2f, 1.1f, 13.9f, -0.5f,
                                           15.5f, 7.5f,  3.6f, 11.4f, 5.9f};
static const float BLING_Y[BLING_COUNT] = {6.2f, 6.8f, 4.3f, 4.7f, 9.6f,
                                           9.9f, 3.5f, 5.1f, 5.4f, 3.9f};
static const uint32_t BLING_DELAY[BLING_COUNT] = {0,   150, 300, 450, 600,
                                                  750, 240, 390, 540, 690};

/** 四角星。**不是圆点**——圆点在这个尺度上只会读成坏点，
 *  两条交叉的尖刺才有"闪"的意思。 */
static void draw_bling(const clawd_canvas_t *canvas, const view_t *v, float x, float y,
                       float s, uint16_t col)
{
    const float lo = 0.11f * s, hi = 0.62f * s;
    put_unit_quad(canvas, v, (const float[]){x - lo, x, x + lo, x},
                  (const float[]){y, y - hi, y, y + hi}, 0.0f, col);
    put_unit_quad(canvas, v, (const float[]){x - hi, x, x + hi, x},
                  (const float[]){y, y - lo, y, y + lo}, 0.0f, col);
}

/** 一个 ♪：符头 + 符干 + 旗子。炫彩现在靠它和台面上的灯来体现。 */
static void draw_note(const clawd_canvas_t *canvas, const view_t *v, float x, float y,
                      float s, uint16_t col)
{
    put_unit_ellipse(canvas, v, x, y, 0.30f * s, 0.24f * s, 0.0f, col);
    put_unit_rect(canvas, v, x + 0.19f * s, y - 1.10f * s, 0.15f * s, 1.16f * s, 0.0f, col);
    const float fx = x + 0.34f * s, fy = y - 1.10f * s;
    put_unit_quad(canvas, v, (const float[]){fx, fx + 0.44f * s, fx + 0.30f * s, fx},
                  (const float[]){fy, fy + 0.30f * s, fy + 0.64f * s, fy + 0.34f * s}, 0.0f,
                  col);
}

#define BLING_PERIOD 1040
#define NOTE_COUNT 4
#define NOTE_LIFE 2080
/* 从台子两侧升起，左右交替，颜色轮着换——炫彩靠它 */
static const float NOTE_X0[NOTE_COUNT] = {14.6f, 0.6f, 15.4f, -0.2f};

static void props_working(const clawd_canvas_t *canvas, const view_t *v, uint32_t t,
                          float bdy)
{
    (void)bdy;
    for (int i = 0; i < BLING_COUNT; i++) {
        const uint32_t ph = (t + BLING_DELAY[i]) % BLING_PERIOD;
        const float f = (float)ph / (float)BLING_PERIOD;
        if (f > 0.42f) continue; /* 大部分时间是灭的，才叫"闪" */
        const float g = f / 0.42f;
        const float sz = sinf(g * 3.1416f); /* 起落对称的一次明灭 */
        if (sz < 0.10f) continue;
        const uint16_t col = (i % 3 == 0) ? LIGHT_B : ((i % 3 == 1) ? VINYL_MARK : LIGHT_A);
        draw_bling(canvas, v, BLING_X[i], BLING_Y[i], sz * 1.15f, col);
    }

    /* 音符从台子两侧飘上去。生灭都靠尺寸而不是透明度：
     * 这个尺度的小图形淡出读不出来，缩小才读得出"飘远了"。 */
    const uint16_t NOTE_COL[4] = {LIGHT_A, LIGHT_B, LIGHT_C, LIGHT_D};
    for (int i = 0; i < NOTE_COUNT; i++) {
        const uint32_t ph = (t + (uint32_t)i * (NOTE_LIFE / NOTE_COUNT)) % NOTE_LIFE;
        const float f = (float)ph / (float)NOTE_LIFE;
        float sz = 1.0f;
        if (f < 0.12f) sz = f / 0.12f;
        else if (f > 0.68f) sz = (1.0f - f) / 0.32f;
        if (sz < 0.12f) continue;
        const float side = (i & 1) ? -1.0f : 1.0f;
        const float x = NOTE_X0[i] + side * (0.9f * f) + sinf(f * 7.0f) * 0.45f;
        const float y = 11.6f - 7.4f * f;
        draw_note(canvas, v, x, y, sz * 1.05f, NOTE_COL[i]);
    }
}

/* --- 完成：一场小型狂欢 --- */

/** 一片翻滚的彩纸：**带角度的矩形**，不是轴对齐的小方块。
 *  原来用 0.22~0.56 宽的正方形，在屏上就是一串抖动的色点，读作坏点；
 *  拉长、给角度、让它边落边翻，才读得出"纸片"。 */
static void draw_ribbon(const clawd_canvas_t *canvas, const view_t *v, float x, float y,
                        float w, float h, float ang, float bdy, uint16_t col)
{
    const float c = cosf(ang), sn = sinf(ang);
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float px[4] = {-hw, hw, hw, -hw};
    const float py[4] = {-hh, -hh, hh, hh};
    float xs[4], ys[4];
    for (int i = 0; i < 4; i++) {
        xs[i] = x + px[i] * c - py[i] * sn;
        ys[i] = y + px[i] * sn + py[i] * c;
    }
    put_unit_quad(canvas, v, xs, ys, bdy, col);
}

#define CONFETTI_COUNT 9
/* **全部落在身体轮廓之外**（躯干占 2..13）。
 * 第一版横向铺满，结果彩纸糊了角色一身，像出疹子。 */
static const float CONF_X[CONFETTI_COUNT] = {-1.7f, -0.9f, -0.1f, 0.7f,
                                             14.2f, 15.0f, 15.8f, 16.6f, 1.5f};
static const uint32_t CONF_DELAY[CONFETTI_COUNT] = {0,   290, 580, 870,
                                                    145, 435, 725, 1015, 1160};

#define SPARK_COUNT 5
static const float SPARK_X[SPARK_COUNT] = {-1.5f, 16.4f, 16.0f, -1.0f, 7.5f};
static const float SPARK_Y[SPARK_COUNT] = {5.4f, 4.8f, 10.6f, 11.4f, 3.6f};
static const uint32_t SPARK_DELAY[SPARK_COUNT] = {0, 260, 520, 780, 1040};
#define SPARK_PERIOD 1300

/*
 * 三层叠在一起才够"大胆"：
 *   1. **顶点爆环**——只在滞空最高点迸发。定时闪的火花跟跳跃没关系，
 *      看着就是背景装饰；卡在顶点上，它才是这一跳"炸出来"的
 *   2. **彩纸下落**——错峰、边落边翻，把整个画面填满
 *   3. **四角星**——补在空档期，和 DJ 态用的是同一套视觉语言
 */
static void props_done(const clawd_canvas_t *canvas, const view_t *v, uint32_t t, float bdy)
{
    const uint32_t ph1s = t % 1000; /* 与 pose_done 的 1s 跳跃周期同相 */

    /* 1) 顶点爆环：40%~60% 是滞空段，50% 是最高点 */
    if (ph1s >= 380 && ph1s < 660) {
        const float k = (float)(ph1s - 380) / 280.0f;
        /*
         * 起始半径要大到**一扩就能露头**。原来从 3.2 起、且光点线性缩到 0，
         * 结果是：小半径时整圈都埋在身体里，等扩出轮廓时点已经缩没了——
         * 爆环等于白画。从 4.5 起、纵向压缩放宽到 0.85（更接近正圆），
         * 光点只从 0.48 收到 0.18 不归零，才看得见。
         */
        const float r = 4.5f + k * 5.5f;
        const float dot = 0.48f - k * 0.30f;
        if (dot > 0.06f) {
            for (int i = 0; i < 10; i++) {
                const float ang = (float)i * 0.6283185f;
                const float x = 7.5f + cosf(ang) * r;
                const float y = 9.0f + sinf(ang) * r * 0.85f;
                /*
                 * **落在身体轮廓里的点直接跳过。**
                 * 爆环从半径 3.2 起扩，前几帧整圈都在躯干（x 2..13、y 6..13）
                 * 里面，光点糊了角色一身——就是"出疹子"那个观感。
                 * 跳过之后，光点只从剪影外面冒出来，读作"从身后炸开"，
                 * 遮挡关系反而把爆发感做实了。
                 */
                if (x > 1.8f && x < 13.2f && y > 5.8f && y < 13.2f) continue;
                put_unit_circle(canvas, v, x, y, dot, bdy, (i & 1) ? SPARK_A : SPARK_B);
            }
        }
    }

    /* 2) 彩纸：从画面上方落下，边落边横向摆、边翻面 */
    for (int i = 0; i < CONFETTI_COUNT; i++) {
        const uint32_t cp = (t + CONF_DELAY[i] * 2) % 2600;
        const float f = (float)cp / 2600.0f;
        const float y = 3.2f + f * 13.2f;
        if (y > 15.6f) continue;
        const float sway = sinf(f * 6.28318f * 2.0f + (float)i) * 0.62f;
        /* 翻面：宽度随相位收缩再张开；角度持续转，两者一起才像纸片在空中翻滚 */
        const float w = 0.34f + 0.52f * fabsf(cosf(f * 6.28318f * 3.0f + (float)i));
        const float ang = f * 6.28318f * 1.6f + (float)i * 0.7f;
        const uint16_t col = (i % 3 == 0) ? SPARK_A : ((i % 3 == 1) ? SPARK_B : BIT_COL);
        draw_ribbon(canvas, v, CONF_X[i] + sway, y, w, 0.30f, ang, bdy, col);
    }

    /* 3) 四角星：一次明灭，起落对称 */
    for (int i = 0; i < SPARK_COUNT; i++) {
        const uint32_t sp = (t + SPARK_DELAY[i]) % SPARK_PERIOD;
        const float f = (float)sp / (float)SPARK_PERIOD;
        if (f > 0.40f) continue;
        const float sz = sinf((f / 0.40f) * 3.1416f);
        if (sz < 0.10f) continue;
        draw_bling(canvas, v, SPARK_X[i], SPARK_Y[i], sz * 1.25f,
                   (i & 1) ? SPARK_B : SPARK_A);
    }
}

/* --- 等待输入：手举起来的灯泡 --- */
static void props_waiting(const clawd_canvas_t *canvas, const view_t *v, uint32_t t, float bdy,
                          float lift)
{
    const float ph = (float)(t % 6000) / 6000.0f;
    if (ph < 0.11f || ph > 0.70f) return; /* 灯泡只在举起的那段出现 */

    const bool lit = (ph >= 0.20f);
    /*
     * **灯泡坐在指尖上，位置是从臂角反解出来的，不是写死的坐标。**
     *
     * 右臂举起时转到 -90°（绕肩关节 (13,10)、臂长 2），手掌那条外边落在
     * x=12..14、y=8，指尖中点就是 (13, 8+lift)。原来把灯泡钉在 x=14.4，
     * 手举在正上方、灯泡却飘在右边——两者差着一整个手掌的宽度，
     * 于是读出来是"举着空气"外加"右上角一个自己在闪的黄块"。
     *
     * 顺序也讲究：灯头压在手上，玻璃泡再叠在灯头上，
     * 三者接触才有"攥住"的感觉。
     */
    const float hand_x = 13.0f, hand_y = 8.0f + lift;
    const float base_h = 0.70f, glass_r = 1.05f;
    const float glass_cy = hand_y - base_h - glass_r * 0.90f;

    /* 灯头留方，玻璃泡用圆——方的读不出"灯泡"，只是一个色块，
     * 两种形状对比才立得住。 */
    put_unit_rect(canvas, v, hand_x - 0.48f, hand_y - base_h, 0.96f, base_h, bdy,
                  lit ? BULB_EDGE_ON : BULB_EDGE_OFF);
    put_unit_circle(canvas, v, hand_x, glass_cy, glass_r, bdy, lit ? BULB_ON : BULB_OFF);
    /* 玻璃上一点高光，灭着的时候尤其需要——纯色圆读不出"玻璃" */
    put_unit_circle(canvas, v, hand_x - glass_r * 0.34f, glass_cy - glass_r * 0.36f,
                    glass_r * 0.22f, bdy,
                    lit ? clawd_rgb565(0xFF, 0xF4, 0xB0) : clawd_rgb565(0xA8, 0xB2, 0xBE));

    if (!lit) return;
    /* 光线：按不规则节拍闪。规则闪烁看着像故障灯，不规则才像"灵光一现"。
     * 全部以玻璃泡中心为原点向外发散——原来是绕着一个固定点发散的，
     * 灯泡一动光线就跟不上了。 */
    const uint32_t beat = (t % 1000);
    if (beat > 620) return;
    const float w = 0.30f;
    for (int i = 0; i < 6; i++) {
        const float ang = 3.1416f + (float)i * 0.6283f; /* 上半圈六道 */
        const float r0 = glass_r + 0.45f, r1 = glass_r + 1.35f;
        const float cx0 = hand_x + cosf(ang) * r0, cy0 = glass_cy + sinf(ang) * r0;
        const float cx1 = hand_x + cosf(ang) * r1, cy1 = glass_cy + sinf(ang) * r1;
        const float nx = -sinf(ang) * w * 0.5f, ny = cosf(ang) * w * 0.5f;
        put_unit_quad(canvas, v,
                      (const float[]){cx0 + nx, cx1 + nx, cx1 - nx, cx0 - nx},
                      (const float[]){cy0 + ny, cy1 + ny, cy1 - ny, cy0 - ny}, bdy, BULB_ON);
    }
}

/* --- 睡觉：鼻子上的呼吸泡泡 ---
 *
 * **不画 Z。** 原来 Z 和泡泡同时存在，两个都在表达"睡着了"，
 * 是重复的；而且四个矩形拼出来的 Z 在这个尺度上边缘全是台阶，
 * 屏上读作乱码而不是字母。留泡泡：它独一份、和呼吸严格同相，
 * 而且鼓大再啵一下破掉本身就是个完整的段子。
 */
static void props_sleeping(const clawd_canvas_t *canvas, const view_t *v, uint32_t t, float bdy)
{
    /*
     * 嘴边的呼吸泡泡：跟着呼吸鼓起来，鼓到最大就啵一下破掉，然后重来。
     * 周期和 pose_sleeping 的 4.5s 呼吸**严格同相**——泡泡自己有节奏的话
     * 就成了两个不相干的动画摆在一起；同相了才是"它在呼吸"。
     */
    /*
     * **泡泡从鼻子出来，长在脸上。**
     * 第一版把它摆在身体右侧的空白里，那就只是一个飘着的白球，
     * 跟角色没有关系。鼻涕泡这个梗的全部笑点就在于它连着鼻孔——
     * 位置错了，再怎么调大小和节奏都不对。
     *
     * 睡姿的脸是躯干正面，闭眼的两道横线在 y=12.5，
     * 鼻子就在两眼之间偏下：x≈7.2, y≈13.7。
     * 吹大时**锚点留在鼻孔**、球心往外推，才像是从那儿鼓出来的，
     * 而不是整颗球在原地放大。
     */
    const float nose_x = 7.2f, nose_y = 13.7f;
    const float bp = (float)(t % 4500) / 4500.0f;
    if (bp < 0.86f) {
        const float grow = bp / 0.86f;
        const float r = 0.16f + grow * grow * 1.05f; /* 先慢后快，像真的在被吹大 */
        /* **往一边吹。** 正对着鼓出来像颗贴在脸上的球；
         * 侧着吹才是"气从鼻孔斜着顶出去"，也才看得出方向。 */
        const float cx = nose_x + r * 1.15f;
        const float cy = nose_y + r * 0.12f;
        put_unit_circle(canvas, v, cx, cy, r, bdy, ZZZ_COL);
        if (r > 0.40f) {
            put_unit_circle(canvas, v, cx - r * 0.34f, cy - r * 0.36f, r * 0.26f, bdy,
                            clawd_rgb565(0xF2, 0xF6, 0xF8));
        }
    } else if (bp < 0.91f) {
        /* 破掉的一瞬：几个碎点从鼻尖向外散 */
        const float k = (bp - 0.86f) / 0.05f;
        for (int i = 0; i < 5; i++) {
            const float ang = (float)i * 1.2566f;
            put_unit_circle(canvas, v, nose_x + 0.7f + cosf(ang) * (1.0f + k * 1.0f),
                            nose_y + 0.3f + sinf(ang) * (1.0f + k * 1.0f),
                            0.18f * (1.0f - k), bdy, ZZZ_COL);
        }
    }

}

/* ------------------------------------------------------------------ *
 * 各状态的动作曲线
 * ------------------------------------------------------------------ */

static void pose_reset(pose_t *p)
{
    p->tap = 0.0f;
    p->ear_press = 0.0f;
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
 * 发呆：呼吸 + 眨眼 + **左右张望**。
 *
 * 只有 2% 的呼吸缩放，在这个尺寸上肉眼根本看不出来——离线渲染出来
 * 六帧一模一样，就是一排静止的方块。加一个 9 秒的张望循环：
 * 往左看两秒、回正、往右看两秒，身体跟着微微侧一点（头眼分离会显得呆）。
 * 眼睛只挪一个单位，但那是整幅画面里唯一在动的东西，"活着"就靠它。
 */
static void pose_idle(pose_t *p, uint32_t t)
{
    apply_breathe_and_blink(p, t);

    /*
     * 周期从 9s 缩到 6.5s，幅度整体放大。
     *
     * 原来的问题不是"没做动画"，是**动作太小又太稀**：眼球位移只有
     * 0.9 个单位（13px 里的一格多），9 秒才走一轮，随便截一帧看都是静止的，
     * 于是读作"这个状态没动画"。发呆可以慢，但不能让人怀疑它死机了。
     */
    const float lp = phase_of(t, 6500);

    static const key_t LOOK[] = {{0.00f, 0.0f},  {0.10f, 0.0f},  {0.16f, -1.5f},
                                 {0.34f, -1.5f}, {0.40f, 0.0f},  {0.54f, 0.0f},
                                 {0.60f, 1.5f},  {0.78f, 1.5f},  {0.84f, 0.0f},
                                 {1.00f, 0.0f}};
    p->eye_dx += ease_keys(LOOK, 10, lp);

    /* 转头时眼睛跟着**微微下沉**——纯水平扫视像玻璃珠在滑，
     * 加一点纵向位移才有"转过去看"的立体感 */
    static const key_t LOOK_Y[] = {{0.00f, 0.0f}, {0.16f, 0.25f}, {0.34f, 0.25f},
                                   {0.40f, 0.0f}, {0.60f, 0.25f}, {0.78f, 0.25f},
                                   {0.84f, 0.0f}, {1.00f, 0.0f}};
    p->eye_dy += ease_keys(LOOK_Y, 8, lp);

    static const key_t LEAN[] = {{0.00f, 0.0f}, {0.16f, -2.2f}, {0.34f, -2.2f},
                                 {0.40f, 0.0f}, {0.60f, 2.2f},  {0.78f, 2.2f},
                                 {0.84f, 0.0f}, {1.00f, 0.0f}};
    p->body_rot += ease_keys(LEAN, 8, lp) * (float)M_PI / 180.0f;

    /*
     * 每一轮末尾抖一下手臂。
     * 一段全靠"缓慢摆动"撑着的循环，看久了还是像卡住——
     * 需要一个**短促、突然**的动作把节奏打断，眼睛才会重新注意到它。
     */
    static const key_t TWITCH[] = {{0.00f, 0.0f}, {0.88f, 0.0f}, {0.91f, -14.0f},
                                   {0.94f, 4.0f}, {0.97f, -6.0f}, {1.00f, 0.0f}};
    const float tw = ease_keys(TWITCH, 6, lp) * (float)M_PI / 180.0f;
    p->arm_l_rot += tw;
    p->arm_r_rot -= tw * 0.6f;
}

/*
 * 干活中：伏案打字。
 * 左右臂周期 150ms / 120ms 互质——双手看起来不同步，这是让它"像在打字"的关键。
 */
static void pose_working(pose_t *p, uint32_t t)
{
    /* 一个节拍统领全部：下面每条曲线都用同一个 beat 或它的整数倍。 */
    const float beat = phase_of(t, BEAT_MS);

    /* 点头：拍点上**沉下去**再弹回。重音在落点而不是顶点——
     * 对称的上下起伏是节拍器，不是音乐。 */
    static const key_t NOD[] = {
        {0.00f, 0.00f}, {0.16f, 0.26f}, {0.50f, -0.06f}, {1.00f, 0.00f}};
    p->body_dy = ease_keys(NOD, 4, beat);

    /* 两拍一个来回的侧身。频率是点头的一半，两个叠起来才有"跟着音乐"的
     * 松弛感；同频的话整个人只是在原地弹。 */
    static const key_t SWAY[] = {{0.00f, -1.8f}, {0.50f, 1.8f}, {1.00f, -1.8f}};
    p->body_rot = ease_keys(SWAY, 3, phase_of(t, BEAT_MS * 2)) * (float)M_PI / 180.0f;

    /* 左手扣着耳罩，每两拍往里压一下——听 mix 的那个下意识动作。
     * 抬手是**正角度**：左臂支点在 (2,10)，负角度是往下甩（那是打字）。 */
    static const key_t PRESS[] = {
        {0.00f, 0.00f}, {0.20f, 1.00f}, {0.52f, 0.18f}, {1.00f, 0.00f}};
    p->ear_press = ease_keys(PRESS, 4, phase_of(t, BEAT_MS * 2));
    /* 74° 是解出来的：手臂外端要正好压在罩子的下半边（罩心 (1.95,7.95) 半径 1.6）。
     * 58° 时外端甩到 x=0.09，掉在罩子左边的黑底上，读出来是块浮空的方块。 */
    p->arm_l_rot = (66.0f + p->ear_press * 7.0f) * (float)M_PI / 180.0f;

    /* 右手搓碟：一拍两下（baby scratch），推出去再拽回来。 */
    static const key_t SCRATCH[] = {
        {0.00f, 0.50f}, {0.22f, 1.00f}, {0.50f, 0.35f}, {0.74f, 0.90f}, {1.00f, 0.50f}};
    p->tap = ease_keys(SCRATCH, 5, beat);
    /* 104°~126° 是按碟的位置反解的：指尖扫过 x≈11.8~12.5、y≈11.6~12.2，
     * 正压在右碟 (11.60, 11.72) 的右半边上。手的位置和碟的转角是
     * **同一个量**驱动的——所以碟的顿挫看起来是被这只手搓出来的，
     * 而不是碟自己在抖。 */
    p->arm_r_rot = (104.0f + p->tap * 22.0f) * (float)M_PI / 180.0f;

    /* 眯着眼享受，偶尔眨一下。**不再左右扫视**——没有屏幕可看了，
     * 留着那段扫视就成了对着空气找东西。 */
    const float ep = phase_of(t, 7000);
    static const key_t EYE_SY[] = {
        {0.00f, 0.55f}, {0.20f, 0.55f}, {0.21f, 0.10f}, {0.23f, 0.55f},
        {0.58f, 0.55f}, {0.59f, 0.10f}, {0.61f, 0.55f}, {1.00f, 0.55f}};
    p->eye_scale_y = ease_keys(EYE_SY, 8, ep);
    /* 眼睛跟着拍子一起沉，幅度只有身体的一半——完全同幅会像整张脸在滑动 */
    p->eye_dy = 0.45f + p->body_dy * 0.5f;

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

    /* 官方那组数是给 30px/单位的画布定的；这里换算成单位量后幅度偏小，
     * 庆祝本来就该夸张，索性再放大一档——跳得起来才叫狂欢。 */
    static const key_t DY[] = {{0.00f, 0.0f},   {0.15f, 0.0f},   {0.20f, 0.0f},
                               {0.40f, -22.0f}, {0.50f, -27.0f}, {0.60f, -22.0f},
                               {0.80f, 0.0f},   {0.85f, 0.0f},   {1.00f, 0.0f}};
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

/*
 * **必须是动画周期（1000ms）的整数倍。**
 * 设成 3600 时正好停在相位 0.6——那一帧人是腾空的（translateY -10），
 * 状态一切就从半空瞬移回地面，看着像"跳完落在了奇怪的位置"。
 * 四个整循环，落地才收。改这里必须同步 sessions.c 的 DONE_HOLD_MS。
 */
bool clawd_done_finished(uint32_t elapsed_ms) { return elapsed_ms >= 4000; }

clawd_rect_t clawd_bounds(const clawd_draw_t *p)
{
    /*
     * 只按**实际占用**的单位范围算，不要用整个 15×16 viewBox——
     * 人物只占 y=6..15 这 9 个单位，上面 6 个单位是空的，
     * 按整格算会白清掉将近一半屏幕，帧耗时直接翻倍。
     *
     * 余量 2 单位覆盖：手臂旋转外甩、DONE 态跳跃、等待态举臂。
     *
     * **别为了让灯铺满屏而把这里调大。** 实测把余量加到 8.5（合成框 477x210）
     * 之后，单帧从 27ms 涨到 116ms——而灯本身只占其中一小部分：
     * 把灯降到半分辨率只省了 15%。真正的开销是那块合成缓冲每帧的
     * 清屏 + 整块拷贝，像素数只涨 1.8 倍，耗时却涨了 4 倍。
     *
     * 要让灯占满屏，正确的做法是**把灯直接画进帧缓冲当背景层**
     * （在 pages.c 里，精灵合成之前），完全绕开这块 scratch 和那次拷贝。
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
            pose_idle(&pose, p->elapsed_ms);
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

    /*
     * 腿（各自带一点颤抖旋转，支点在腿根）。
     *
     * **打碟时不画腿**：人站在台子后面，腿完整地被台面挡着，
     * 唯一能让它露出来的情况是身体跟着拍子下沉时脚穿过地面线——
     * 那是穿帮不是效果。不画既去掉穿帮，也省四个图元。
     */
    for (int i = 0; i < (p->state == CLAWD_WORKING ? 0 : 4); i++) {
        const rect_t *leg = &legs[i];
        transform_rect(&view, leg, leg->x + leg->w * 0.5f, leg->y, pose.leg_rot[i],
                       pose.body_form == 2 ? pose.body_sx : 1.0f,
                       pose.body_form == 2 ? pose.body_sy : 1.0f, bdx, bdy, quad);
        fill_quad(canvas, quad, p->body_color);
    }

    /* 躯干 */
    BODY_XFORM(torso);
    fill_quad(canvas, quad, p->body_color);


    /*
     * 合成器和耳机都画在**躯干之后、手臂之前**：
     * 琴横在身前、罩子扣在头侧，两只手都落在它们上面。
     * 顺序反了就成了"人躲在琴后面"和"手藏在耳罩里面"。
     *
     * 手的横坐标由臂角直接算出来（肩关节 (13,10)、臂长 2），
     * 亮起的琴键就是这么跟手对上的——不是另起一个计时器去闪。
     */
    if (p->state == CLAWD_WORKING) {
        props_decks(canvas, &view, p->elapsed_ms, (pose.tap - 0.5f) * 2.0f, p->body_color);
        props_headphones(canvas, &view, bdy, pose.ear_press);
    }

    /* 双臂。**比躯干暗一档**——摆到身前时同色会整条融进身体，
     * 于是"手按在琴键上"读成"身上多了一块"。 */
    const uint16_t arm_col = shade565(p->body_color, 0.86f);
    if (pose.body_form == 2) {
        /* 睡姿的手是摊在地上的，跟着身体一起缩放，不单独旋转 */
        BODY_XFORM(arm_l);
        fill_quad(canvas, quad, arm_col);
        BODY_XFORM(arm_r);
        fill_quad(canvas, quad, arm_col);
    } else {
        transform_rect(&view, arm_l, ARM_L_PIVOT_X, ARM_L_PIVOT_Y, pose.arm_l_rot, 1.0f,
                       1.0f, bdx, bdy, quad);
        fill_quad(canvas, quad, arm_col);
        transform_rect(&view, arm_r, ARM_R_PIVOT_X, ARM_R_PIVOT_Y, pose.arm_r_rot, 1.0f,
                       1.0f, bdx, bdy + pose.arm_r_dy, quad);
        fill_quad(canvas, quad, arm_col);
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
        case CLAWD_WORKING:
            props_working(canvas, &view, p->elapsed_ms, bdy);
            break;
        case CLAWD_DONE: props_done(canvas, &view, p->elapsed_ms, bdy); break;
        case CLAWD_WAITING: props_waiting(canvas, &view, p->elapsed_ms, bdy, pose.arm_r_dy); break;
        case CLAWD_SLEEPING: props_sleeping(canvas, &view, p->elapsed_ms, bdy); break;
        default: break;
    }

#undef BODY_XFORM
}
