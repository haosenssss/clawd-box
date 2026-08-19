/*
 * 会话表 —— 板子侧的全部业务状态。
 *
 * **全静态分配，稳态零 malloc。** 协议是"状态替换"而非"事件累积"，
 * 所以内存是 O(会话数 × 定长)，结构上不可能随运行时间增长。
 *
 * 实算：单条 240B × 8 = 约 2KB，而帧缓冲是 900KB。
 * 真正吃内存的是帧缓冲，不是信息量。
 */
#pragma once

#include "sprite/clawd.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_SESSIONS 8
#define MAX_SUBAGENTS 16
#define SESSION_ID_LEN 9  /* 8 字符 + NUL */
#define SESSION_NAME_LEN 32
#define AGENT_ID_LEN 9

typedef enum { SESS_IDLE = 0, SESS_BUSY, SESS_WAITING } session_status_t;

typedef struct {
    char id[AGENT_ID_LEN];
    bool done;
    bool used;
} subagent_t;

typedef struct {
    bool used;
    char id[SESSION_ID_LEN];
    char name[SESSION_NAME_LEN];
    session_status_t status;

    /* 上下文窗口占用 0-100，负数=无数据。**按会话**，不是账号级。 */
    float ctx_pct;

    /* 板子本地时钟（毫秒），用于计时与陈旧判定 */
    uint32_t updated_ms;
    uint32_t status_since_ms;

    /* 一次性状态：刚完成。播完由仲裁器清掉。 */
    uint32_t done_at_ms;
    bool done_pending;

    /* 等待输入的退避提醒 */
    uint32_t waiting_since_ms;
    uint32_t last_remind_ms;
    uint8_t remind_step;
    bool remind_acked;

    subagent_t subagents[MAX_SUBAGENTS];
} session_t;

typedef struct {
    float pct;       /* 0-100，负数表示无数据 */
    int64_t resets_at; /* unix 秒，0 表示未给 */
} limit_window_t;

typedef struct {
    limit_window_t five_hour;
    limit_window_t seven_day;
    bool cached;        /* true=来自缓存兜底，UI 应暗色渲染 */
    uint32_t age_sec;
} limits_t;

/** 全部状态。单例，由 link 层写、UI 层读。 */
typedef struct {
    session_t sessions[MAX_SESSIONS];
    limits_t limits;
    /* 主机 hello 帧带来的 unix 秒，用于把本地毫秒换算成墙上时间 */
    int64_t host_unix_sec;
    uint32_t host_sync_ms;
    bool linked;
    uint32_t last_frame_ms;
} model_t;

void model_init(model_t *m);

/** 找到会话；不存在时按需创建（表满则淘汰最久未更新的）。 */
session_t *model_touch_session(model_t *m, const char *id, uint32_t now_ms);
session_t *model_find_session(model_t *m, const char *id);
void model_remove_session(model_t *m, const char *id);

void model_sub_start(session_t *s, const char *agent_id);
void model_sub_stop(session_t *s, const char *agent_id);
/** 一轮结束——清空圆点。 */
void model_clear_subagents(session_t *s);

int model_sub_total(const session_t *s);
int model_sub_done(const session_t *s);

/** 注册的会话总数（含空闲的，管理页用）。 */
int model_active_count(const model_t *m);

/*
 * 轮播环 —— **只收录"正在干活"和"等待输入"两类**，外加刚完成还在谢幕的。
 * 空闲会话不占页：终端开着但没在用的，不该抢走屏幕。
 */
int model_ring_count(const model_t *m, uint32_t now_ms);
session_t *model_ring_at(model_t *m, int index, uint32_t now_ms);

/**
 * 抢占式聚焦：**恰好一个**会话在干活时返回它，否则 NULL。
 * 此时停止轮播，屏幕锁定在这个唯一在跑的会话上——正在思考/操作的那个，
 * 才是你此刻真正想盯着的。两个以上同时在跑就没有"唯一"可言，回到轮播。
 */
session_t *model_sole_busy(model_t *m, uint32_t now_ms);

/** 会话当前应该演哪个动作。 */
clawd_state_t model_clawd_state(const session_t *s, uint32_t now_ms);
