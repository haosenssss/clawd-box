/*
 * 输入层：把触摸手势和两个实体键归一成同一串事件。
 *
 * 上层不该关心"这一下是滑的还是按的"——导航规则只有三条，
 * 事件也就只有三个。多一个都是给自己找麻烦。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    INPUT_NONE = 0,
    /** 去左边——**永远是管理页**，不管当前停在哪一页 */
    INPUT_GO_LEFT,
    /** 去右边——下一个会话页 */
    INPUT_GO_RIGHT,
    /** 确认：静音当前提醒。视觉状态照旧保留，只是不再出声 */
    INPUT_ACK,
} input_event_t;

/** 需要先 bsp_board_init()。触摸初始化失败不算致命，实体键仍可用。 */
esp_err_t input_init(void);

/** 每帧调一次。没有输入时返回 INPUT_NONE。 */
input_event_t input_poll(uint32_t now_ms);
