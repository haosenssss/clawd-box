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
#define OUT_VOLUME 78

/* 一段提示音最长 2.2 秒。留足余量给最后那个长音的余韵——
 * 缓冲不够的话尾音会被硬切，听起来就是"响到一半没了"。 */
#define MAX_MS 2200
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

/*
 * ms 是**往后推进的时间**，不是音符的长度（长度由余韵决定）。于是：
 *   {440, 200}  普通音，响完往后走 200ms
 *   {0,   120}  休止，只推进时间
 *   {440, 0}    **和弦音**：和下一个音同时起振，自己不推进时间
 *   {0,   0}    结束
 * 和弦是这次加的——单音一个个排下去只能是"嘀嘀嘀"，
 * 收尾处几个音一起响才有"落定"的分量。
 */
typedef struct {
    float freq;
    uint16_t ms;
} note_t;

/*
 * 音阶用**大调五声**（C D E G A）。
 *
 * 五声音阶里任意两个音叠在一起都不会出现刺耳的小二度和三全音，
 * 所以听感天然是温和的、不紧张的——这正是 Anthropic 那种审美要的东西：
 * 不抢戏、不像游戏音效、不像系统报警。和弦感来自音程本身，
 * 而不是靠加音量或加合成器花活。
 */
/* 完成：C5 - E5 - G5 - C6 上行四音，最后一个音留长做余韵。
 * 原来三个短音总共 280ms，听起来像"嘀"了一下就没了；
 * 拉到 700ms 并让尾音自然衰减，才有"一件事收尾了"的分量。 */
static const note_t SEQ_DONE[] = {
    {523.25f, 95},  {659.25f, 95},  {783.99f, 110}, {880.00f, 130},
    /* 落回高八度主音，同时点亮下方的 E5 和 G5——一个完整的 C 和弦收尾。
     * 五声音阶里这三个音叠在一起不会有任何刺耳的音程。 */
    {659.25f, 0},   {783.99f, 0},   {1046.50f, 700},
    {0, 0}};

/* 等你输入：A5 - D6 上行两次。上行=询问，重复=需要你动手。
 * 两遍之间留一点静默，像敲两下门而不是连成一串。 */
static const note_t SEQ_NEEDS[] = {
    {880.00f, 120}, {1174.66f, 175}, {0.0f, 115},
    {880.00f, 120}, {1174.66f, 175}, {0.0f, 115},
    /* 第三遍收在 A5+D6 上（纯四度，稳）。用 D6+E6 会撞出大二度，
     * 那是"警报"的听感，不是"叫你一声"。 */
    {880.00f, 110}, {880.00f, 0}, {1174.66f, 420},
    {0, 0}};

/* 限额告急：G4 - E4 - C4 下行三音。下行且低，是"出问题"的通用听感。
 * 放慢、放长，让它听起来像一声叹息而不是警报。 */
static const note_t SEQ_LIMIT[] = {
    {392.00f, 175}, {329.63f, 175}, {293.66f, 175},
    /* 落到 G3+C4，低而空，像一口叹出去的气 */
    {196.00f, 0},   {261.63f, 760},
    {0, 0}};

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
 * 三个决定听感的细节：
 *
 * **一、指数衰减而不是方波包络。** 现实里被敲响的东西都是"起得快、落得慢"，
 * 线性收尾听起来像被人为掐断。exp 衰减是钢琴、木琴、钟这类声音的共同特征。
 *
 * **二、叠一个二次谐波。** 纯正弦干净但也单薄、发闷。加一点点八度泛音
 * （幅度只有基频的 1/5）就有了木质的温度，又不至于变刺耳。
 *
 * **三、音符之间不清相位、尾音互相重叠。** 前一个音的尾巴还没落干净，
 * 下一个音就进来了，听起来是一串连贯的琶音而不是三个孤立的"嘀"。
 */
static size_t render(const note_t *seq, int16_t *out, size_t cap)
{
    size_t n = 0;
    /* 先清零：后面是叠加写入，尾音要能跨过音符边界 */
    for (size_t i = 0; i < cap; i++) out[i] = 0;

    size_t at = 0;
    for (const note_t *note = seq; !(note->freq == 0.0f && note->ms == 0); note++) {
        const size_t step = (size_t)SAMPLE_RATE * note->ms / 1000;
        if (note->freq <= 0.0f) { /* 静默间隔只推进时间 */
            at += step;
            if (at > n) n = at;
            continue;
        }

        /* 尾音比音符本身长——重叠出来的就是"余韵"。
         * 和弦音 step=0，靠这里的固定余量才能响满。 */
        const size_t tail = step + (size_t)SAMPLE_RATE * 430 / 1000;
        const float w = 2.0f * (float)M_PI * note->freq / (float)SAMPLE_RATE;
        /* 起振 6ms：再短会咔哒，再长会发软 */
        const size_t attack = (size_t)SAMPLE_RATE * 6 / 1000;
        const float decay = 3.2f / (float)tail;

        for (size_t i = 0; i < tail; i++) {
            const size_t idx = at + i;
            if (idx >= cap) break;
            float env = expf(-decay * (float)i);
            if (i < attack) env *= (float)i / (float)attack;

            const float ph = w * (float)i;
            const float v = sinf(ph) + 0.20f * sinf(ph * 2.0f);
            /* 渲染阶段**刻意留足余量**（5000 够五个音同时响不削顶），
             * 响度交给下面的归一化统一处理。 */
            const int32_t sample = (int32_t)out[idx] + (int32_t)(v * env * 5000.0f);
            out[idx] = (int16_t)(sample > 32000 ? 32000 : (sample < -32000 ? -32000 : sample));
            if (idx + 1 > n) n = idx + 1;
        }
        at += step;
    }

    /*
     * 归一化到接近满刻度。
     *
     * **不要靠调那个幅度常数去凑响度。** 音符一多、和弦一叠，
     * 峰值就翻倍，之前调好的常数立刻削顶——失真听起来是"破音"，
     * 比声音小难受得多。渲染时留余量、最后统一拉到目标峰值，
     * 每段提示音的响度才既一致又拉满。
     */
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t a = out[i] < 0 ? -(int32_t)out[i] : (int32_t)out[i];
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        const float g = 26000.0f / (float)peak;
        for (size_t i = 0; i < n; i++) out[i] = (int16_t)((float)out[i] * g);
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

    /* 110 KB 必须走 PSRAM——内部 RAM 总共才 300 KB 上下，
     * 拿去放一段提示音会把渲染的离屏缓冲挤掉。 */
    s_buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_buf == NULL) s_buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_DEFAULT);
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
