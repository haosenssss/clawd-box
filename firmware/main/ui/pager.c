#include "pager.h"

#include <string.h>

/* 轮播停留时长。计划里是 5~10 秒，取中间值。 */
#define ROTATE_MS 7000
/* 手动操作后暂停自动轮播的时长 */
#define MANUAL_HOLD_MS 30000

void pager_init(pager_t *p, uint32_t now_ms)
{
    memset(p, 0, sizeof(*p));
    p->kind = PAGE_SESSION;
    p->focus_since = now_ms;
}

static void remember(pager_t *p, const session_t *s)
{
    if (s == NULL) { p->last_focus_id[0] = '\0'; return; }
    strncpy(p->last_focus_id, s->id, SESSION_ID_LEN - 1);
    p->last_focus_id[SESSION_ID_LEN - 1] = '\0';
}

/** 上次看的那个会话现在在环里的第几位；找不到返回 0。 */
static int index_of_remembered(const pager_t *p, model_t *m, uint32_t now_ms)
{
    if (p->last_focus_id[0] == '\0') return 0;
    const int n = model_ring_count(m, now_ms);
    for (int i = 0; i < n; i++) {
        const session_t *s = model_ring_at(m, i, now_ms);
        if (s != NULL && strncmp(s->id, p->last_focus_id, SESSION_ID_LEN - 1) == 0) return i;
    }
    return 0;
}

bool pager_input(pager_t *p, model_t *m, input_event_t e, uint32_t now_ms)
{
    if (e == INPUT_NONE) return false;

    /* 任何手动操作都推迟自动轮播——包括确认，你正在看着它 */
    p->manual_until = now_ms + MANUAL_HOLD_MS;

    switch (e) {
        case INPUT_GO_LEFT:
            if (p->kind == PAGE_ADMIN) return false;
            remember(p, model_ring_at(m, p->ring_index, now_ms));
            p->kind = PAGE_ADMIN;
            p->focus_since = now_ms;
            return true;

        case INPUT_GO_RIGHT: {
            if (p->kind == PAGE_ADMIN) {
                p->kind = PAGE_SESSION;
                p->ring_index = index_of_remembered(p, m, now_ms);
                p->focus_since = now_ms;
                return true;
            }
            const int n = model_ring_count(m, now_ms);
            if (n <= 1) return false; /* 没有别的页可去，别做无意义的动画 */
            p->ring_index = (p->ring_index + 1) % n;
            p->focus_since = now_ms;
            return true;
        }

        case INPUT_ACK:
            /*
             * 确认 **≠ 消除**：只是不再出声，视觉状态照旧持续显示，
             * 状态本身消失才算完。所以这里不算"页面变了"。
             * 屏幕上有几个在等就一起确认——你已经知道了。
             */
            for (int i = 0; i < MAX_SESSIONS; i++) {
                session_t *s = model_at(m, i);
                if (s == NULL) break;
                if (s->status == SESS_WAITING) model_reminder_ack(s);
            }
            return false;

        case INPUT_NONE:
        default:
            return false;
    }
}

session_t *pager_tick(pager_t *p, model_t *m, uint32_t now_ms)
{
    if (p->kind == PAGE_ADMIN) return NULL;

    const int n = model_ring_count(m, now_ms);
    if (n == 0) {
        p->ring_index = 0;
        return NULL;
    }
    if (p->ring_index >= n) p->ring_index = 0;

    /* 手动操作后的静默期内，完全听用户的 */
    if ((int32_t)(p->manual_until - now_ms) > 0) {
        return model_ring_at(m, p->ring_index, now_ms);
    }

    /*
     * 抢占优先于轮播：**恰好一个**会话在干活时锁定它。
     * 那一刻你想盯着的就是它，没有什么可轮的。
     */
    session_t *sole = model_sole_busy(m, now_ms);
    if (sole != NULL) {
        for (int i = 0; i < n; i++) {
            if (model_ring_at(m, i, now_ms) == sole) { p->ring_index = i; break; }
        }
        p->focus_since = now_ms; /* 抢占期间冻结轮播计时 */
        return sole;
    }

    if (n >= 2 && (uint32_t)(now_ms - p->focus_since) > ROTATE_MS) {
        p->ring_index = (p->ring_index + 1) % n;
        p->focus_since = now_ms;
    }
    return model_ring_at(m, p->ring_index, now_ms);
}
