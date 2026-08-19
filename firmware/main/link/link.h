/* 串口链路对外接口。 */
#pragma once

#include "esp_err.h"
#include "model/sessions.h"

/** 启动接收任务。model 生命周期须长于整个程序。 */
esp_err_t link_start(model_t *model);

/** 累计丢弃的行数（超长 / 解析失败）。用于健康观测。 */
uint32_t link_dropped(void);
