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

static void put_span(const clawd_canvas_t *c, int y, int x0, int x1, uint16_t color)
{
    if (y < 0 || y >= c->height) return;
    if (x0 < 0) x0 = 0;
    if (x1 > c->width - 1) x1 = c->width - 1;
    if (x0 > x1) return;
    uint16_t *line = c->pixels + (size_t)y * c->width;
    for (int x = x0; x <= x1; x++) line[x] = color;
}

static void fill_quad(const clawd_canvas_t *c, const pt_t q[4], uint16_t color)
{
    float min_y = q[0].y, max_y = q[0].y;
    for (int i = 1; i < 4; i++) {
        if (q[i].y < min_y) min_y = q[i].y;
        if (q[i].y > max_y) max_y = q[i].y;
    }
    int y0 = (int)floorf(min_y);
    int y1 = (int)ceilf(max_y);
    if (y1 < 0 || y0 > c->height - 1) return;
    if (y0 < 0) y0 = 0;
    if (y1 > c->height - 1) y1 = c->height - 1;

    for (int y = y0; y <= y1; y++) {
        const float sy = (float)y + 0.5f;
        float xs[4];
        int n = 0;
        for (int i = 0; i < 4 && n < 4; i++) {
            const pt_t a = q[i];
            const pt_t b = q[(i + 1) & 3];
            if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) {
                const float t = (sy - a.y) / (b.y - a.y);
                xs[n++] = a.x + t * (b.x - a.x);
            }
        }
        if (n < 2) continue;
        float lo = xs[0], hi = xs[0];
        for (int i = 1; i < n; i++) {
            if (xs[i] < lo) lo = xs[i];
            if (xs[i] > hi) hi = xs[i];
        }
        put_span(c, y, (int)floorf(lo), (int)ceilf(hi) - 1, color);
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
    p->body_dy = ease_keys(BOUNCE, 3, phase_of(t, 350)) * 0.06f;

    static const key_t TYPE[] = {
        {0.00f, -5.0f}, {0.25f, -38.0f}, {0.50f, -10.0f}, {0.75f, -30.0f}, {1.00f, -5.0f}};
    p->arm_l_rot = ease_keys(TYPE, 5, phase_of(t, 150)) * (float)M_PI / 180.0f;
    p->arm_r_rot = -ease_keys(TYPE, 5, phase_of(t, 120)) * (float)M_PI / 180.0f;

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
    const float wave = ease_keys(WAVE, 3, phase_of(t, 150)) * (float)M_PI / 180.0f;
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

#undef BODY_XFORM
}
