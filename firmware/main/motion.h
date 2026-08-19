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
