/*
 * 重力 → 精灵位移。
 *
 * 刻意做得克制：**这不是水平仪**。倾斜时精灵朝低处偏一点、晃一下就停，
 * 传达的是"它有重量"，不是"这里是多少度"。位移封顶 ±24px，
 * 再大就会盖住上面的圆点和下面的名字，变成干扰而不是细节。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** 位移上限（整数版，给需要按像素扩区域的调用方用） */
#define MOTION_MAX_PX_I 24

typedef struct {
    float x, y;   /* 当前位移（像素） */
    float vx, vy; /* 速度 */
    float bx, by;  /* 静止姿态基线（慢跟随），吸收"板子是怎么摆的" */
    float prev_mag;
    bool have_prev;
    bool have_base;
} motion_t;

void motion_init(motion_t *m);

/**
 * 推进一步。没有 IMU 时传 false，位移会平滑回零而不是突然复位。
 * 返回 true 表示这一步检测到了"晃动"，可以让上层触发一次弹跳。
 */
bool motion_step(motion_t *m, bool have_accel, float ax, float ay, float dt);

/*
 * 沿边框爬行。
 *
 * 把屏幕边框当成一条闭合轨道，重力在**切线方向**上的分量推着它走：
 * 你把板子往哪边歪，它就顺着边往那边的低处滑，滑到最低点停住。
 * 法线方向的分量被边框挡住，所以它不会掉下来——这正是"贴着边爬"的物理。
 *
 * 只在没有活动会话时启用：会话页上精灵四周排着圆点、项目名和三条额度栏，
 * 让它满屏跑会把那些全压掉。没任务时那一页本来就是空的，正好给它当场地。
 */
typedef struct {
    float s;         /* 沿周长的位置（像素） */
    float v;         /* 切向速度（像素/秒） */
    bool started;
} walk_t;

void walk_init(walk_t *w);

/**
 * 推进一步并给出落点。
 * @param inset 轨道距屏幕边缘的内缩量（精灵的半高，免得半个身子出界）
 * @param out_rot 朝向（弧度）——脚要踩在边上，四条边各差 90 度
 */
void walk_step(walk_t *w, bool have_accel, float ax, float ay, float dt, int screen_w,
               int screen_h, int inset, float *out_x, float *out_y, float *out_rot);
