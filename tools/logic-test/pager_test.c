/*
 * 轮播/抢占逻辑的确定性测试。
 *
 * 这类逻辑全是时序条件（庆祝保持 4s、轮播 7s、抢占、静默期 30s），
 * 在硬件上靠肉眼撞——庆祝只有 4 秒而状态行 10 秒才打一次，
 * 根本看不全。时间是 pager_tick() 的入参，原生跑就能完全掌控。
 */
#include "model/sessions.h"
#include "ui/pager.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

static void check(int cond, const char *what)
{
    printf("  %s %s\n", cond ? "✓" : "✗ 失败:", what);
    if (!cond) g_fail++;
}

/** 把会话设成"正在干活" */
static void set_busy(model_t *m, const char *id, uint32_t t)
{
    session_t *s = model_touch_session(m, id, t);
    s->status = SESS_BUSY;
    s->status_since_ms = t;
    s->done_pending = false;
}

/** 把会话设成"刚干完"——等价于收到 turn_end */
static void set_done(model_t *m, const char *id, uint32_t t)
{
    session_t *s = model_touch_session(m, id, t);
    s->status = SESS_IDLE;
    s->status_since_ms = t;
    s->done_pending = true;
    s->done_at_ms = t;
}

static const char *focus_id(pager_t *p, model_t *m, uint32_t t)
{
    session_t *f = pager_tick(p, m, t);
    return f ? f->id : "(无)";
}

int main(void)
{
    model_t m;
    pager_t p;
    uint32_t t = 100000;

    memset(&m, 0, sizeof(m));
    pager_init(&p, t);

    printf("场景：A、B 同时在跑，A 先完成，B 仍在跑\n");
    set_busy(&m, "A", t);
    set_busy(&m, "B", t);
    m.last_frame_ms = t;
    m.linked = true;

    check(model_ring_count(&m, t) == 2, "两个会话都在轮播环里");

    /* 让焦点先停在 A 上 */
    while (strcmp(focus_id(&p, &m, t), "A") != 0 && t < 130000) t += 500;
    check(strcmp(focus_id(&p, &m, t), "A") == 0, "焦点先停在 A");

    /* A 完成，B 还在跑 —— 抢占规则此刻会认为"恰好一个在忙"=B */
    set_done(&m, "A", t);
    const uint32_t done_at = t;

    check(model_clawd_state(model_find_session(&m, "A"), t) == CLAWD_DONE,
          "A 进入庆祝状态");

    /* 庆祝全程：焦点必须钉在 A 上 */
    int held = 1;
    for (uint32_t dt = 0; dt < 3900; dt += 100) {
        if (strcmp(focus_id(&p, &m, done_at + dt), "A") != 0) { held = 0; break; }
    }
    check(held, "庆祝的整整 4 秒里焦点不让位（这就是被修掉的 bug）");

    /* 庆祝演完之后，才轮到 B 抢占 */
    t = done_at + 4200;
    check(strcmp(focus_id(&p, &m, t), "B") == 0, "庆祝结束后焦点交给仍在跑的 B");

    printf("\n场景：只有一个会话在跑时锁定它，不做无谓轮播\n");
    memset(&m, 0, sizeof(m));
    pager_init(&p, t);
    set_busy(&m, "C", t);
    m.linked = true;
    check(strcmp(focus_id(&p, &m, t + 9000), "C") == 0, "单会话恒定锁定，不轮播");

    printf("\n%s\n", g_fail == 0 ? "全部通过" : "有失败项");
    return g_fail;
}

/* pager_input() 会读触摸落点来决定管理页点了哪一行；本测试不涉及输入，
 * 给个桩满足链接即可。 */
int16_t input_tap_y(void) { return 0; }
