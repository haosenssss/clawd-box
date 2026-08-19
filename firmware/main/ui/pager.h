/*
 * 导航状态机。
 *
 * 规则只有三条，刻意做得没有例外：
 *   1. **左边永远是管理页**——不管当前停在哪个会话页
 *   2. 右边是下一个会话页；从管理页往右回到刚才看的那个
 *   3. 手动操作后暂停自动轮播 30 秒，你的意图优先于系统的
 *
 * 自动行为里，"恰好一个会话在干活"会抢占并锁屏；否则在环里轮播。
 */
#pragma once

#include "input/input.h"
#include "model/sessions.h"
#include <stdbool.h>

typedef enum { PAGE_SESSION = 0, PAGE_ADMIN } page_kind_t;

typedef struct {
    page_kind_t kind;
    int ring_index;
    uint32_t focus_since;
    /** 手动操作后自动轮播暂停到这个时刻 */
    uint32_t manual_until;
    /** 从管理页往右回来时落回的会话 */
    char last_focus_id[SESSION_ID_LEN];
} pager_t;

void pager_init(pager_t *p, uint32_t now_ms);

/** 处理一个输入事件。返回 true 表示页面变了，需要整屏重绘。 */
bool pager_input(pager_t *p, model_t *m, input_event_t e, uint32_t now_ms);

/** 每帧调。返回当前该显示的会话；管理页或环空时返回 NULL。 */
session_t *pager_tick(pager_t *p, model_t *m, uint32_t now_ms);
