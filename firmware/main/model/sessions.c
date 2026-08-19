#include "sessions.h"

#include <string.h>

/* 超过这个时长没更新就移出轮播（管理页仍可灰显）。
 * 实测本机 6 个注册会话里 4 个是 2~19 天前的"停车"会话，
 * 主机侧已过滤一层，这里再兜一层，防止断链期间残留。 */
#define ACTIVE_WINDOW_MS (12U * 60U * 60U * 1000U)

/* 一次性状态的最短保持时长——防止状态抖动导致动画闪烁 */
#define DONE_HOLD_MS 1200U

/* 闲置多久算睡着 */
#define IDLE_TO_SLEEP_MS 60000U

void model_init(model_t *m)
{
    memset(m, 0, sizeof(*m));
}

static bool id_eq(const char *a, const char *b)
{
    return strncmp(a, b, SESSION_ID_LEN - 1) == 0;
}

session_t *model_find_session(model_t *m, const char *id)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (m->sessions[i].used && id_eq(m->sessions[i].id, id)) return &m->sessions[i];
    }
    return NULL;
}

session_t *model_touch_session(model_t *m, const char *id, uint32_t now_ms)
{
    session_t *s = model_find_session(m, id);
    if (s != NULL) {
        s->updated_ms = now_ms;
        return s;
    }

    /* 找空位 */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!m->sessions[i].used) {
            s = &m->sessions[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            strncpy(s->id, id, SESSION_ID_LEN - 1);
            s->updated_ms = now_ms;
            s->status_since_ms = now_ms;
            s->ctx_pct = -1.0f;
            return s;
        }
    }

    /* 满了——淘汰最久未更新的（LRU） */
    session_t *oldest = &m->sessions[0];
    for (int i = 1; i < MAX_SESSIONS; i++) {
        if (m->sessions[i].updated_ms < oldest->updated_ms) oldest = &m->sessions[i];
    }
    memset(oldest, 0, sizeof(*oldest));
    oldest->used = true;
    strncpy(oldest->id, id, SESSION_ID_LEN - 1);
    oldest->updated_ms = now_ms;
    oldest->status_since_ms = now_ms;
    oldest->ctx_pct = -1.0f;
    return oldest;
}

void model_remove_session(model_t *m, const char *id)
{
    session_t *s = model_find_session(m, id);
    if (s != NULL) memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ *
 * subagent 记账
 *
 * 运行数无法从任何文件推断（meta 文件完成后不删除，运行态只在 Claude Code
 * 的内存里），只能靠 SubagentStart/Stop 事件自己记。会话消失时整条清空——
 * 硬杀进程不会触发 SubagentStop，不清就会永远留着空心圆点。
 * ------------------------------------------------------------------ */

void model_sub_start(session_t *s, const char *agent_id)
{
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (s->subagents[i].used && id_eq(s->subagents[i].id, agent_id)) return;
    }
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (!s->subagents[i].used) {
            s->subagents[i].used = true;
            s->subagents[i].done = false;
            strncpy(s->subagents[i].id, agent_id, AGENT_ID_LEN - 1);
            s->subagents[i].id[AGENT_ID_LEN - 1] = '\0';
            return;
        }
    }
    /* 满了：覆盖最老的已完成项；若全部未完成则丢弃（UI 显示 +N） */
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (s->subagents[i].done) {
            s->subagents[i].done = false;
            strncpy(s->subagents[i].id, agent_id, AGENT_ID_LEN - 1);
            s->subagents[i].id[AGENT_ID_LEN - 1] = '\0';
            return;
        }
    }
}

void model_sub_stop(session_t *s, const char *agent_id)
{
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (s->subagents[i].used && id_eq(s->subagents[i].id, agent_id)) {
            s->subagents[i].done = true;
            return;
        }
    }
}

void model_clear_subagents(session_t *s)
{
    memset(s->subagents, 0, sizeof(s->subagents));
}

int model_sub_total(const session_t *s)
{
    int n = 0;
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (s->subagents[i].used) n++;
    }
    return n;
}

int model_sub_done(const session_t *s)
{
    int n = 0;
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (s->subagents[i].used && s->subagents[i].done) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ *
 * 轮播环
 * ------------------------------------------------------------------ */

static bool is_active(const session_t *s, uint32_t now_ms)
{
    if (!s->used) return false;
    return (uint32_t)(now_ms - s->updated_ms) < ACTIVE_WINDOW_MS;
}

int model_active_count(const model_t *m)
{
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (m->sessions[i].used) n++;
    }
    return n;
}

/*
 * 能进轮播环的三类：正在干活、等待输入、刚完成还在谢幕。
 *
 * 空闲会话被排除掉——终端开着但没在用的不该占页。谢幕窗口是有界的
 * （不是看 done_pending 标志），因为主机只会为**当前聚焦**的会话清那个标志，
 * 非聚焦会话的标志没人清，用它当条件会让完成过的会话永远赖在环里。
 */
#define DONE_LINGER_MS 4000U

static bool in_ring(const session_t *s, uint32_t now_ms)
{
    if (!is_active(s, now_ms)) return false;
    if (s->done_pending && (uint32_t)(now_ms - s->done_at_ms) < DONE_LINGER_MS) return true;
    return s->status == SESS_BUSY || s->status == SESS_WAITING;
}

int model_ring_count(const model_t *m, uint32_t now_ms)
{
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (in_ring(&m->sessions[i], now_ms)) n++;
    }
    return n;
}

session_t *model_ring_at(model_t *m, int index, uint32_t now_ms)
{
    /*
     * 按数组槽位顺序返回。槽位是首次出现时分配的，所以先出现的会话
     * 永远排在前面——页序稳定，绝不会在用户手指底下重排。
     */
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!in_ring(&m->sessions[i], now_ms)) continue;
        if (n == index) return &m->sessions[i];
        n++;
    }
    return NULL;
}

session_t *model_at(model_t *m, int index)
{
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!m->sessions[i].used) continue;
        if (n == index) return &m->sessions[i];
        n++;
    }
    return NULL;
}

/* 退避档位：进入等待立即响一次，之后间隔逐级拉长，封顶 5 分钟 */
static const uint32_t REMIND_GAP_MS[] = {0, 30000, 60000, 120000, 300000};
#define REMIND_STEPS (sizeof(REMIND_GAP_MS) / sizeof(REMIND_GAP_MS[0]))

bool model_reminder_due(session_t *s, uint32_t now_ms)
{
    if (s == NULL || s->status != SESS_WAITING || s->remind_acked) return false;

    if (s->last_remind_ms == 0) { /* 刚进入等待，立即响 */
        s->last_remind_ms = now_ms;
        s->remind_step = 1;
        return true;
    }
    const uint8_t idx = s->remind_step < REMIND_STEPS ? s->remind_step : REMIND_STEPS - 1;
    if ((uint32_t)(now_ms - s->last_remind_ms) < REMIND_GAP_MS[idx]) return false;

    s->last_remind_ms = now_ms;
    if (s->remind_step < REMIND_STEPS - 1) s->remind_step++;
    return true;
}

void model_reminder_ack(session_t *s)
{
    if (s != NULL) s->remind_acked = true;
}

session_t *model_sole_busy(model_t *m, uint32_t now_ms)
{
    session_t *found = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        session_t *s = &m->sessions[i];
        if (!is_active(s, now_ms) || s->status != SESS_BUSY) continue;
        if (found != NULL) return NULL; /* 不止一个，谈不上"唯一" */
        found = s;
    }
    return found;
}

clawd_state_t model_clawd_state(const session_t *s, uint32_t now_ms)
{
    if (s == NULL || !s->used) return CLAWD_SLEEPING;

    /* 一次性的"刚完成"优先，且有最短保持时长 */
    if (s->done_pending && (uint32_t)(now_ms - s->done_at_ms) < DONE_HOLD_MS) {
        return CLAWD_DONE;
    }
    switch (s->status) {
        case SESS_WAITING: return CLAWD_WAITING;
        case SESS_BUSY: return CLAWD_WORKING;
        case SESS_IDLE:
        default:
            /*
             * **闲久了就该睡着。**
             * 原来只有"一个会话都没有"时才演睡姿，于是从管理页点进一个
             * 早就收工的项目，看到的还是站着发呆——那个姿势是给"刚停下"
             * 准备的，用在一个几小时没动静的项目上就显得没道理。
             * 一分钟没动静就摊平睡觉，和人的直觉一致。
             */
            return (uint32_t)(now_ms - s->status_since_ms) > IDLE_TO_SLEEP_MS
                       ? CLAWD_SLEEPING
                       : CLAWD_IDLE;
    }
}
