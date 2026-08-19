/*
 * 提示音。
 *
 * 不放音频资源，正弦振荡器 + 包络实时合成，零 flash 占用。
 * 听感原则：**上行=完成/询问，重复=需要你动手，下行且低=出问题**。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    SOUND_DONE = 0,   /* 任务完成：上行三音，只响一次——这是通知不是催促 */
    SOUND_NEEDS_YOU,  /* 等你输入：叩门式重复双音，按退避节奏反复响 */
    SOUND_LIMIT,      /* 限额告急：下行低音，只响一次 */
} sound_t;

/** 失败不致命——没有喇叭时整个界面照常工作，只是不出声。 */
esp_err_t audio_init(void);

/** 非阻塞。播放中再来一个会被丢弃，提示音不该排队堆积。 */
void audio_play(sound_t s);

void audio_set_muted(bool muted);
bool audio_muted(void);
