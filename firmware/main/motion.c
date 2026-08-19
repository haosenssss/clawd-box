#include "motion.h"

#include <math.h>

/* 位移上限。再大就会压到上面的 subagent 圆点和下面的项目名。 */
#define MOTION_MAX_PX ((float)MOTION_MAX_PX_I)

/*
 * 二阶弹簧阻尼。ζ=0.7 是轻微欠阻尼——会过冲一点点再停住，
 * 这一下过冲正是"有质量"的观感来源；ζ=1 看起来像被拖着走，很死。
 */
#define OMEGA 8.0f
#define ZETA 0.7f

/* 晃动判定：合加速度在一步之内的变化量。1g 是明显甩了一下，手放上去不会触发。 */
#define SHAKE_DELTA_G 1.0f
/* 晃动给的速度冲量 */
#define SHAKE_IMPULSE 260.0f

/* 基线跟随的时间常数。太快会把慢慢倾斜也吸收掉，太慢则换姿势后要等很久才回正。 */
#define BASE_TAU_S 3.0f
/* 相对静止姿态偏离多少 g 算"打满" */
#define TILT_FULL_G 0.5f

void motion_init(motion_t *m)
{
    m->x = m->y = m->vx = m->vy = 0.0f;
    m->bx = m->by = 0.0f;
    m->prev_mag = 1.0f;
    m->have_prev = false;
    m->have_base = false;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool motion_step(motion_t *m, bool have_accel, float ax, float ay, float dt)
{
    if (dt <= 0.0f) return false;
    /* 单步时长封顶：掉帧时 dt 会突然变大，不封的话弹簧会数值发散 */
    dt = clampf(dt, 0.0f, 0.05f);

    float tx = 0.0f, ty = 0.0f;
    bool shook = false;

    if (have_accel) {
        /*
         * **对静止姿态取相对值，不是对地心取绝对值。**
         *
         * 直接用重力方向的话，板子一旦竖着装（86 盒本来就是墙面形态），
         * 某个轴就长期是 1g，精灵会被永久按在位移上限上——那不是细节，那是歪了。
         * 这里用一个慢跟随的基线吸收掉"当前是怎么摆的"，
         * 只对**偏离静止姿态**的部分做出反应：怎么装都居中，一碰才动。
         */
        if (!m->have_base) {
            m->bx = ax;
            m->by = ay;
            m->have_base = true;
        } else {
            const float a = dt / (BASE_TAU_S + dt); /* 一阶低通 */
            m->bx += (ax - m->bx) * a;
            m->by += (ay - m->by) * a;
        }
        /* 归一化到"半个 g 就打满"，不必大幅倾斜就有明显反馈 */
        tx = clampf((ax - m->bx) / TILT_FULL_G, -1.0f, 1.0f) * MOTION_MAX_PX;
        ty = clampf((ay - m->by) / TILT_FULL_G, -1.0f, 1.0f) * MOTION_MAX_PX;

        const float mag = sqrtf(ax * ax + ay * ay);
        if (m->have_prev && fabsf(mag - m->prev_mag) > SHAKE_DELTA_G) {
            shook = true;
            m->vy -= SHAKE_IMPULSE; /* 向上弹一下 */
        }
        m->prev_mag = mag;
        m->have_prev = true;
    } else {
        m->have_prev = false; /* 没数据就回零，别把最后一帧的倾斜一直挂着 */
    }

    const float k = OMEGA * OMEGA;
    const float c = 2.0f * ZETA * OMEGA;
    m->vx += (k * (tx - m->x) - c * m->vx) * dt;
    m->vy += (k * (ty - m->y) - c * m->vy) * dt;
    m->x = clampf(m->x + m->vx * dt, -MOTION_MAX_PX, MOTION_MAX_PX);
    m->y = clampf(m->y + m->vy * dt, -MOTION_MAX_PX, MOTION_MAX_PX);
    return shook;
}

/* ------------------------------------------------------------------ *
 * 沿边框爬行
 * ------------------------------------------------------------------ */

/* 重力到切向加速度的换算。数值定得让"歪 45 度"能在两秒内走完一条边。 */
#define WALK_GAIN 900.0f
/* 阻尼。太小会在最低点来回荡个不停，太大就推不动。 */
#define WALK_DAMP 2.6f
#define WALK_MAX_V 420.0f

void walk_init(walk_t *w)
{
    w->s = 0.0f;
    w->v = 0.0f;
    w->started = false;
}

void walk_step(walk_t *w, bool have_accel, float ax, float ay, float dt, int screen_w,
               int screen_h, int inset, float *out_x, float *out_y, float *out_rot)
{
    const float W = (float)(screen_w - 2 * inset);
    const float H = (float)(screen_h - 2 * inset);
    const float P = 2.0f * (W + H);
    if (P <= 0.0f) return;

    if (!w->started) {
        /* 从底边中点出发——那是"站在地上"的默认姿势 */
        w->s = W + H + W * 0.5f;
        w->started = true;
    }

    if (dt > 0.05f) dt = 0.05f;

    /* 当前所在的边，以及该边的切线方向（沿 s 增大的方向） */
    float s = fmodf(w->s, P);
    if (s < 0.0f) s += P;

    float tx, ty;
    if (s < W)                { tx = 1.0f;  ty = 0.0f;  }   /* 顶边，向右 */
    else if (s < W + H)       { tx = 0.0f;  ty = 1.0f;  }   /* 右边，向下 */
    else if (s < 2.0f * W + H){ tx = -1.0f; ty = 0.0f;  }   /* 底边，向左 */
    else                      { tx = 0.0f;  ty = -1.0f; }   /* 左边，向上 */

    if (have_accel) {
        /*
         * **只取切向分量。** 法向的那一半被边框接住了——
         * 这就是为什么它贴着边走而不是掉下去。
         * 屏幕的 y 轴向下，重力 az 为负时板子正面朝上；
         * 这里只用 x/y 两轴，正好对应屏幕平面内的倾斜。
         */
        const float at = (ax * tx + ay * ty) * WALK_GAIN;
        w->v += (at - WALK_DAMP * w->v) * dt;
    } else {
        w->v -= WALK_DAMP * w->v * dt;
    }
    if (w->v > WALK_MAX_V) w->v = WALK_MAX_V;
    if (w->v < -WALK_MAX_V) w->v = -WALK_MAX_V;
    w->s = s + w->v * dt;

    s = fmodf(w->s, P);
    if (s < 0.0f) s += P;

    /* 落点与朝向：脚踩在边上，四条边各差 90 度 */
    if (s < W) {
        *out_x = (float)inset + s;
        *out_y = (float)inset;
        *out_rot = (float)M_PI;             /* 顶边：倒挂 */
    } else if (s < W + H) {
        *out_x = (float)(screen_w - inset);
        *out_y = (float)inset + (s - W);
        *out_rot = -(float)M_PI / 2.0f;     /* 右边：脚朝右 */
    } else if (s < 2.0f * W + H) {
        *out_x = (float)(screen_w - inset) - (s - W - H);
        *out_y = (float)(screen_h - inset);
        *out_rot = 0.0f;                    /* 底边：正常站立 */
    } else {
        *out_x = (float)inset;
        *out_y = (float)(screen_h - inset) - (s - 2.0f * W - H);
        *out_rot = (float)M_PI / 2.0f;      /* 左边：脚朝左 */
    }
}
