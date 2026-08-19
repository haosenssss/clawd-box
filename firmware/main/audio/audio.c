#include "audio.h"

#include "board_bsp.h"
#include "board_config.h"

#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

static const char *TAG = "audio";

#define SAMPLE_RATE BSP_AUDIO_SAMPLE_RATE
#define OUT_VOLUME 70

/* 一段提示音最长 500ms。够放三个音符，也不至于长到烦人。 */
#define MAX_MS 500
#define TONE_SAMPLES (SAMPLE_RATE * MAX_MS / 1000)

/*
 * 音符放完必须再灌一段静音，**否则声音会一直循环响下去**。
 *
 * I2S 通道一旦 enable，DMA 就在那几个描述符之间无限轮转。停止写入
 * 并不会让它停下来——它只是把缓冲里**上一次的内容反复播**，
 * 于是提示音变成了没完没了的循环。
 * 灌够一整圈 DMA 缓冲的静音，环里就全是 0 了。
 * 默认 6 个描述符 × 240 帧 = 1440 帧，取 3000 留足余量。
 */
#define SILENCE_SAMPLES 3000
#define MAX_SAMPLES (TONE_SAMPLES + SILENCE_SAMPLES)

typedef struct {
    float freq;   /* Hz，0 = 静音间隔 */
    uint16_t ms;
} note_t;

/* 上行三音 C5-E5-G5：短促、明亮、结束感明确 */
static const note_t SEQ_DONE[] = {{523.25f, 80}, {659.25f, 80}, {783.99f, 120}, {0, 0}};
/* 叩门式重复双音：同一个音重复本身就在说"喂"，比任何旋律都直接 */
static const note_t SEQ_NEEDS[] = {{987.77f, 90}, {0.0f, 70}, {987.77f, 90}, {0, 0}};
/* 下行且低：出问题的通用听感 */
static const note_t SEQ_LIMIT[] = {{392.00f, 130}, {261.63f, 200}, {0, 0}};

static const note_t *sequence_for(sound_t s)
{
    switch (s) {
        case SOUND_NEEDS_YOU: return SEQ_NEEDS;
        case SOUND_LIMIT: return SEQ_LIMIT;
        case SOUND_DONE:
        default: return SEQ_DONE;
    }
}

static esp_codec_dev_handle_t s_dev = NULL;
static i2s_chan_handle_t s_tx = NULL;
static QueueHandle_t s_queue = NULL;
static int16_t *s_buf = NULL;
static bool s_muted = false;

void audio_set_muted(bool muted) { s_muted = muted; }
bool audio_muted(void) { return s_muted; }

/**
 * 把一串音符渲染成 PCM。
 *
 * 每个音符都套一个短起振 + 短收尾的包络：**不加包络就会有咔哒声**，
 * 因为波形在非零处被硬切断，那一下阶跃比音符本身还响。
 */
static size_t render(const note_t *seq, int16_t *out, size_t cap)
{
    size_t n = 0;
    float phase = 0.0f;

    for (const note_t *note = seq; note->ms != 0; note++) {
        const size_t len = (size_t)SAMPLE_RATE * note->ms / 1000;
        const size_t edge = len / 8 > 0 ? len / 8 : 1; /* 起振/收尾各占 1/8 */

        for (size_t i = 0; i < len && n < cap; i++, n++) {
            if (note->freq <= 0.0f) { out[n] = 0; continue; }

            float env = 1.0f;
            if (i < edge) env = (float)i / (float)edge;
            else if (i > len - edge) env = (float)(len - i) / (float)edge;

            phase += 2.0f * (float)M_PI * note->freq / (float)SAMPLE_RATE;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            out[n] = (int16_t)(sinf(phase) * env * 9000.0f);
        }
        phase = 0.0f;
    }
    return n;
}

static void audio_task(void *arg)
{
    (void)arg;
    sound_t s;
    while (true) {
        if (xQueueReceive(s_queue, &s, portMAX_DELAY) != pdTRUE) continue;
        if (s_muted || s_dev == NULL) continue;
        size_t n = render(sequence_for(s), s_buf, TONE_SAMPLES);
        if (n == 0) continue;

        ESP_LOGI(TAG, "播放 %d（%u 采样）", (int)s, (unsigned)n);

        /* 音符之后紧跟静音，把 DMA 环里的旧内容冲掉——见 SILENCE_SAMPLES */
        memset(s_buf + n, 0, SILENCE_SAMPLES * sizeof(int16_t));
        n += SILENCE_SAMPLES;

        /*
         * **放完就把通道关掉。**
         * 光靠补静音还不够保险：只要 I2S 通道是 enable 的，DMA 就在描述符之间
         * 无限轮转，任何残留都会被反复播出去——症状就是提示音响个没完。
         * 关掉通道等于把数据流掐断，物理上不可能再出声；下次播放前再打开。
         */
        i2s_channel_enable(s_tx);
        esp_codec_dev_write(s_dev, s_buf, n * sizeof(int16_t));
        i2s_channel_disable(s_tx);
    }
}

static esp_err_t i2s_init(i2s_chan_handle_t *tx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, tx, NULL), TAG, "建通道失败");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_BCLK,
            .ws = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    /* 初始化完**不使能**——空闲时通道必须是关着的，见 audio_task */
    return i2s_channel_init_std_mode(*tx, &std_cfg);
}

esp_err_t audio_init(void)
{
    ESP_RETURN_ON_ERROR(i2s_init(&s_tx), TAG, "I2S 初始化失败");

    const audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .tx_handle = s_tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_PORT,
        .addr = BSP_ADDR_ES8311 << 1, /* 组件要的是 8 位地址 */
        .bus_handle = bsp_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) return ESP_ERR_NOT_FOUND;

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, /* 本板没有独立的功放使能脚 */
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (codec_if == NULL) return ESP_ERR_NOT_FOUND;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (s_dev == NULL) return ESP_ERR_NO_MEM;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 1,
        .sample_rate = SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_dev, &fs), TAG, "打开编解码器失败");
    esp_codec_dev_set_out_vol(s_dev, OUT_VOLUME);

    s_buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (s_buf == NULL) return ESP_ERR_NO_MEM;

    s_queue = xQueueCreate(2, sizeof(sound_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    /* 钉到 CPU1：渲染在 CPU0，合成和 I2S 写入不该和它抢 */
    if (xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ES8311 就绪，%d Hz", SAMPLE_RATE);
    return ESP_OK;
}

void audio_play(sound_t s)
{
    if (s_queue == NULL || s_muted) return;
    /* 满了就丢——提示音排队堆积只会变成噪音 */
    xQueueSend(s_queue, &s, 0);
}
