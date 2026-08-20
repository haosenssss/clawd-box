/*
 * 串口链路：从 UART0 读主机推来的换行分隔 JSON，更新会话表。
 *
 * 与控制台共用 UART0。板子只解析以 '{' 开头的行，日志照常输出到同一个口，
 * 主机侧只写不读——两个方向互不干扰，且开发期还能看日志。
 *
 * 超长行整行丢弃并计数，绝不试图截断后解析——半截 JSON 比丢一帧危险得多。
 */

#include "link.h"

#include "cJSON.h"
#include "driver/uart.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "link";

#define LINK_UART UART_NUM_0
/* 环形缓冲要能吃下一整轮 resync 的突发（hello + 全部会话 + limits + mlimit），
 * 而不是刚好装下一帧。渲染循环偶尔会让链路任务停摆几十毫秒，
 * 这段时间的字节全靠这个缓冲兜住。8KB 在 8MB PSRAM 面前不值一提。 */
#define RX_BUF_SIZE 8192
#define LINK_LINE_MAX 512 /* 与主机侧 MAX_FRAME_BYTES 一致 */

static model_t *s_model = NULL;
static uint32_t s_dropped = 0;

/*
 * 板子有两个 USB 口：CH343P 桥（走 UART0）和 ESP32-S3 自带的 USB-Serial-JTAG。
 * 日志两个口都会输出（ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG），
 * 于是插错口时"看得见日志、收不到帧"，现象酷似死机——实际只是没人读那个口。
 * 两个口都收，插哪个都能用。
 *
 * 每个来源一份独立的行装配器：两路字节流绝不能拼进同一个缓冲区，
 * 否则半行 A 接上半行 B，解析出来的是彻头彻尾的假数据。
 *
 * **绝不能用 usb_serial_jtag_driver_install()。** 试过，代价惨重：
 * IDF 的次级控制台（ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG）本来就占着这个外设，
 * 装驱动后控制台输出改走驱动的 TX 环形缓冲并会阻塞，
 * 渲染单帧从 27ms 一路劣化到 114ms，几分钟后日志彻底卡死。
 * 这里只轮询硬件 RX FIFO：不注册中断、不碰 TX 通路，
 * 而且 RX 方向本来就没有第二个读者，不存在争用。
 */
typedef struct {
    char buf[LINK_LINE_MAX];
    size_t len;
    bool overflow;
} line_asm_t;

static line_asm_t s_uart_line;
static line_asm_t s_usb_line;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static double json_num(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static session_status_t parse_status(const char *s)
{
    if (s == NULL) return SESS_IDLE;
    if (strcmp(s, "busy") == 0) return SESS_BUSY;
    if (strcmp(s, "waiting") == 0) return SESS_WAITING;
    return SESS_IDLE; /* 未知值按开放枚举降级 */
}

static void set_status(session_t *s, session_status_t next, uint32_t t)
{
    if (s->status == next) return;
    s->status = next;
    s->status_since_ms = t;
    if (next == SESS_WAITING) {
        s->waiting_since_ms = t;
        s->last_remind_ms = 0;
        s->remind_step = 0;
        s->remind_acked = false;
    }
}

static void apply_frame(const cJSON *root)
{
    const char *e = json_str(root, "e");
    if (e == NULL) return;

    const uint32_t t = now_ms();
    s_model->last_frame_ms = t;
    s_model->linked = true;

    if (strcmp(e, "hello") == 0) {
        s_model->host_unix_sec = (int64_t)json_num(root, "ts", 0);
        s_model->host_sync_ms = t;
        ESP_LOGI(TAG, "主机握手，对时 %lld", (long long)s_model->host_unix_sec);
        return;
    }

    if (strcmp(e, "limits") == 0) {
        const cJSON *h5 = cJSON_GetObjectItemCaseSensitive(root, "h5");
        const cJSON *w7 = cJSON_GetObjectItemCaseSensitive(root, "w7");
        s_model->limits.five_hour.pct = cJSON_IsNumber(h5) ? (float)h5->valuedouble : -1.0f;
        s_model->limits.seven_day.pct = cJSON_IsNumber(w7) ? (float)w7->valuedouble : -1.0f;
        s_model->limits.five_hour.resets_at = (int64_t)json_num(root, "h5r", 0);
        s_model->limits.seven_day.resets_at = (int64_t)json_num(root, "w7r", 0);
        const char *src = json_str(root, "src");
        s_model->limits.cached = (src != NULL && strcmp(src, "cached") == 0);
        s_model->limits.age_sec = (uint32_t)json_num(root, "age", 0);
        return;
    }

    const char *id = json_str(root, "id");
    if (id == NULL) return;

    if (strcmp(e, "session") == 0) {
        session_t *s = model_touch_session(s_model, id, t);
        const char *name = json_str(root, "name");
        if (name != NULL) {
            strncpy(s->name, name, SESSION_NAME_LEN - 1);
            s->name[SESSION_NAME_LEN - 1] = '\0';
        }
        set_status(s, parse_status(json_str(root, "status")), t);
        return;
    }

    if (strcmp(e, "session_gone") == 0) {
        model_remove_session(s_model, id);
        return;
    }

    session_t *s = model_touch_session(s_model, id, t);

    if (strcmp(e, "prompt") == 0) {
        set_status(s, SESS_BUSY, t);
        s->done_pending = false;
        s->done_chimed = false;
    } else if (strcmp(e, "turn_end") == 0) {
        /* 一轮结束：进入"刚完成"一次性状态，并清空 subagent 圆点 */
        s->done_pending = true;
        s->done_chimed = false; /* 新的一次完成，允许再响一声 */
        s->done_at_ms = t;
        model_clear_subagents(s);
        set_status(s, SESS_IDLE, t);
    } else if (strcmp(e, "idle_prompt") == 0) {
        set_status(s, SESS_WAITING, t);
    } else if (strcmp(e, "sub_start") == 0) {
        const char *aid = json_str(root, "aid");
        if (aid != NULL) model_sub_start(s, aid);
    } else if (strcmp(e, "ctx") == 0) {
        s->ctx_pct = (float)json_num(root, "pct", -1.0);
    } else if (strcmp(e, "sub_stop") == 0) {
        const char *aid = json_str(root, "aid");
        if (aid != NULL) model_sub_stop(s, aid);
    }
}

static void handle_line(line_asm_t *a)
{
    if (a->overflow) {
        a->overflow = false;
        a->len = 0;
        s_dropped++;
        return;
    }
    if (a->len == 0) return;
    a->buf[a->len] = '\0';

    /* 只认 JSON 行，其余（比如我们自己的日志回显）直接忽略 */
    if (a->buf[0] == '{') {
        cJSON *root = cJSON_Parse(a->buf);
        if (root != NULL) {
            ESP_LOGD(TAG, "帧: %s", a->buf);
            apply_frame(root);
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "解析失败(%u字节): %.80s", (unsigned)a->len, a->buf);
            s_dropped++;
        }
    } else {
        ESP_LOGD(TAG, "非JSON行(%u字节): %.40s", (unsigned)a->len, a->buf);
    }
    a->len = 0;
}

static void feed(line_asm_t *a, const uint8_t *data, int n)
{
    for (int i = 0; i < n; i++) {
        const char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            handle_line(a);
            continue;
        }
        if (a->len < LINK_LINE_MAX - 1) {
            a->buf[a->len++] = c;
        } else {
            a->overflow = true; /* 整行作废，等换行符时丢弃 */
        }
    }
}

static void link_task(void *arg)
{
    (void)arg;
    uint8_t chunk[256];
    uint32_t total_bytes = 0;
    uint32_t report_at = 0;
    while (true) {
        /* UART 阻塞 20ms 兼作整个循环的节拍；USB-JTAG 非阻塞轮询，
         * 这样任一路来数据都不会被另一路的等待拖住。 */
        const int n = uart_read_bytes(LINK_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (n > 0) {
            total_bytes += (uint32_t)n;
            feed(&s_uart_line, chunk, n);
        }
        /* 自带 USB 口：直接把硬件 RX FIFO 抽干，一次最多 64 字节 */
        while (usb_serial_jtag_ll_rxfifo_data_available()) {
            const uint32_t m = usb_serial_jtag_ll_read_rxfifo(chunk, 64);
            if (m == 0) break;
            total_bytes += m;
            feed(&s_usb_line, chunk, (int)m);
        }
        const uint32_t t = now_ms();
        if (total_bytes > 0 && t - report_at > 3000) {
            report_at = t;
            ESP_LOGI(TAG, "累计收到 %lu 字节", (unsigned long)total_bytes);
        }
    }
}

esp_err_t link_start(model_t *model)
{
    s_model = model;

    /*
     * 只安装驱动以便读取，**不碰 uart_param_config**——波特率由控制台配置统一决定。
     * 曾经在这里重配过波特率，结果是日志从某一行开始变乱码，
     * 现象很像崩溃、实际只是收发两端不一致，很容易误诊。
     */
    /*
     * **中断优先级必须显式抬到 LEVEL3。**
     *
     * RGB 屏的 bounce buffer 中断每秒跑上千次、每次从 PSRAM 搬两万字节，
     * 长期占着中断上下文。默认优先级下 UART 的接收中断会被它压住——实测
     * 主机侧两帧间隔已经是准确的 40ms，板子仍然会把跨越这两帧的
     * **约 130 字节**（正好一个硬件 FIFO）整段丢掉：
     *     发出: {"e":"session",...,"name":"hquant-57","status":"idle","u":…}
     *           {"e":"session",...,"name":"clawd-hardware-status-panel",…,"u":1787164933}
     *     收到: {"e":"session",...,"name":"hquant-57"933}
     * 前一帧的头 + 后一帧的尾，中间连换行符一起消失——这是
     * RX FIFO 溢出后驱动整体 flush 的特征，不是波特率或线序问题。
     *
     * ESP_INTR_FLAG_IRAM 同样不能省：处理函数留在 flash 里的话，
     * 一次 cache miss 就足以错过排空 FIFO 的时机。
     */
    esp_err_t err = uart_driver_install(LINK_UART, RX_BUF_SIZE, 0, 0, NULL,
                                        ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3);
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_FOUND) {
        /* 该优先级已被占用时退回默认，宁可丢帧也不能起不来 */
        ESP_LOGW(TAG, "LEVEL3 中断不可用，退回默认优先级");
        err = uart_driver_install(LINK_UART, RX_BUF_SIZE, 0, 0, NULL, 0);
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    /* 早一点触发排空，别等到 FIFO 快满（默认 120/128）才动 */
    uart_set_rx_full_threshold(LINK_UART, 40);

    /* 钉到 CPU1：渲染循环跑在 CPU0 且密集读写 PSRAM，同核会把链路任务饿住。 */
    if (xTaskCreatePinnedToCore(link_task, "link", 4096, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "串口链路就绪：UART0 + USB-JTAG（沿用控制台波特率）");
    return ESP_OK;
}

uint32_t link_dropped(void) { return s_dropped; }
