/*
 * Clawd Box 主循环。
 *
 * 板子是"厚"的一侧：会话表、计时、状态仲裁、页面轮播都在这里，
 * Mac 只负责把它读不到的东西（注册表、PID 存活、钩子事件）转发过来。
 */

#include <stdio.h>
#include <string.h>

#include "bsp/board_bsp.h"
#include "bsp/board_config.h"
#include "link/link.h"
#include "model/sessions.h"
#include "sprite/clawd.h"
#include "sprite/text.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- 官方配色（Claude Code theme.ts / Anthropic 品牌 token）---- */
/* 纯黑。Clawd 的官方背景 token clawd_background 就是 rgb(0,0,0)；
 * 之前用 #141413 是把 Anthropic 的页面背景色用错了地方。
 * 纯黑还让橙色主体和 IPS 屏的对比度都达到最好。 */
#define COL_BG clawd_rgb565(0x00, 0x00, 0x00)
#define COL_BODY clawd_rgb565(0xD7, 0x77, 0x57)    /* clawd_body */
#define COL_EYE clawd_rgb565(0x00, 0x00, 0x00)     /* clawd_background */
#define COL_TEXT clawd_rgb565(0xF0, 0xEE, 0xE6)    /* --swatch--ivory-medium，数据用 */
#define COL_NAME clawd_rgb565(0xB0, 0xAE, 0xA5)    /* --swatch--cloud-medium，项目名 */
#define COL_VERB clawd_rgb565(0x78, 0x76, 0x70)    /* 状态词，更暗一档 */
#define COL_RESET clawd_rgb565(0x87, 0x86, 0x7F)   /* --swatch--cloud-dark，重置时间 */
#define COL_DIM clawd_rgb565(0x99, 0x99, 0x99)     /* inactive */
#define COL_SUBTLE clawd_rgb565(0x50, 0x50, 0x50)  /* subtle */
#define COL_FILL clawd_rgb565(0x57, 0x69, 0xF7)    /* rate_limit_fill */
#define COL_EMPTY clawd_rgb565(0x27, 0x2F, 0x6F)   /* rate_limit_empty */
#define COL_WARN clawd_rgb565(0xFF, 0xC1, 0x07)    /* warning */
#define COL_CRIT clawd_rgb565(0xFF, 0x6B, 0x80)    /* error */
#define COL_OK clawd_rgb565(0x4E, 0xBA, 0x65)      /* success */

#define TARGET_FPS 30
#define FRAME_MS (1000 / TARGET_FPS)
#define ROTATE_MS 7000

#define SPRITE_SCALE 18.0f   /* 15 单位宽 -> 270px，与额度条同宽同轴 */
#define SPRITE_BASELINE_Y 262

/* ---- 布局 ---- */
#define DOTS_Y 38
#define DOT_R 7
#define DOT_GAP 26
#define NAME_Y 290           /* 第一行：项目名 */
#define VERB_Y 322           /* 第二行：状态词 */
#define BAR_LABEL_CX 50       /* 标签列中心（0..条子左缘 之间居中） */
#define BAR_X 105              /* = 精灵左边缘，两者必须等宽同轴 */
#define BAR_W 270              /* = 精灵宽度 15 单位 x 18 */
#define BAR_H 16
/* 百分比列**右对齐**：这一列的值宽度不一（5% / 42% / 100%），
 * 左对齐会让各行的 % 号错开成锯齿。右边缘对齐后 % 永远在同一条竖线上。 */
#define BAR_PCT_R 429
#define BAR_RESET_X 439
#define BAR_Y0 358
#define BAR_DY 42
#define BAR_ROWS 3
/* 文字 7 单位高，比条子矮，垂直居中到条子里 */
#define BAR_TEXT_DY ((BAR_H - 7 * TEXT_SCALE) / 2)
#define TEXT_SCALE 2

static model_t s_model;

/*
 * 精灵区离屏合成缓冲。
 *
 * bounce buffer 模式下写帧缓冲是**立即可见**的，所以不能在帧缓冲里
 * "先擦后画"——那个中间态会被看到，就是闪动。
 * 改成：在 scratch 里擦+画，再整块拷过去。拷贝是一次线性写入，
 * 屏幕上看不到黑底中间态。
 */
static uint16_t *s_scratch = NULL;
static uint32_t s_frame_ms = 0;
static int s_scratch_w = 0;
static int s_scratch_h = 0;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 官方 189 个动词里挑的短款，便于在 480 宽内和项目名同行显示 */
static const char *VERBS[] = {
    "Cogitating", "Percolating", "Noodling",  "Pondering", "Brewing",
    "Churning",   "Crafting",    "Simmering", "Musing",    "Whisking",
    "Clauding",   "Mulling",     "Forging",   "Hatching",  "Puzzling",
    "Vibing",     "Working",     "Thinking",  "Swirling",  "Tinkering",
};
#define NVERBS (sizeof(VERBS) / sizeof(VERBS[0]))

/** 按会话 id + 本轮起始时刻取一个动词——同一轮内稳定，换轮才变。 */
static const char *verb_for(const session_t *s)
{
    uint32_t h = s->status_since_ms / 1000u;
    for (const char *p = s->id; *p != '\0'; p++) h = h * 31u + (unsigned char)*p;
    return VERBS[h % NVERBS];
}

/**
 * 重置时间写成**倒计时**而不是官方那种绝对时刻（"3pm"）。
 * 主机只传了 UTC 秒、没传时区，绝对时刻会显示错；倒计时免疫时区，
 * 而且"还有多久"本来就比"几点"更好用。
 */
static void format_countdown(char *out, size_t n, int64_t resets_at, int64_t now_unix)
{
    if (resets_at <= 0 || now_unix <= 0) { out[0] = '\0'; return; }
    int64_t left = resets_at - now_unix;
    if (left <= 0) { snprintf(out, n, "due"); return; }

    /*
     * **只保留最大的那个单位**（"3d" / "2h" / "45m"），不写 "2h05m"。
     * 一是分钟级精度对 5 小时/7 天的窗口毫无意义，二是这一列每多两个字符，
     * 上面的精灵和额度条就得同步变窄——横向预算是死的，字省下来的都归了图形。
     */
    const int64_t days = left / 86400;
    const int64_t hours = left / 3600;
    if (days > 0)       snprintf(out, n, "%lldd", (long long)days);
    else if (hours > 0) snprintf(out, n, "%lldh", (long long)hours);
    else                snprintf(out, n, "%lldm", (long long)(left / 60) + 1);
}

/**
 * 居中绘制，宽度超了就自动降一档字号；仍超则末尾用 ".." 截断。
 * 会话名长度不可控（实测有 "opinion-level-attribution-research" 这种），
 * 不做保护就会画到屏幕外。
 */
static void draw_fit_center(const text_canvas_t *tc, int cx, int y, const char *s,
                            int max_w, int scale, uint16_t color)
{
    if (text_width(s, scale) <= max_w) {
        text_draw_center(tc, cx, y, s, scale, color);
        return;
    }
    if (scale > 1 && text_width(s, scale - 1) <= max_w) {
        text_draw_center(tc, cx, y, s, scale - 1, color);
        return;
    }
    /* 还是放不下：按字符数截断并加 ".." */
    const int use = scale > 1 ? scale - 1 : scale;
    const int per = TEXT_ADVANCE(use);
    int fit = (max_w - text_width("..", use)) / per;
    if (fit < 1) fit = 1;
    char buf[64];
    const int n = fit < (int)sizeof(buf) - 3 ? fit : (int)sizeof(buf) - 3;
    memcpy(buf, s, (size_t)n);
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '\0';
    text_draw_center(tc, cx, y, buf, use, color);
}

/* ------------------------------------------------------------------ */

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BSP_LCD_H_RES) w = BSP_LCD_H_RES - x;
    if (y + h > BSP_LCD_V_RES) h = BSP_LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        uint16_t *line = fb + (size_t)row * BSP_LCD_H_RES + x;
        for (int i = 0; i < w; i++) line[i] = c;
    }
}

static void clear_all(uint16_t *fb, uint16_t c)
{
    const uint32_t pair = ((uint32_t)c << 16) | c;
    uint32_t *p = (uint32_t *)fb;
    for (size_t i = 0; i < (size_t)BSP_LCD_H_RES * BSP_LCD_V_RES / 2; i++) p[i] = pair;
}

static void draw_dot(uint16_t *fb, int cx, int cy, int r, uint16_t c, bool filled)
{
    const int r2 = r * r;
    const int inner2 = (r - 2) * (r - 2);
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            if (!filled && d2 < inner2) continue;
            const int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= BSP_LCD_H_RES || y < 0 || y >= BSP_LCD_V_RES) continue;
            fb[(size_t)y * BSP_LCD_H_RES + x] = c;
        }
    }
}

/**
 * 一根限额条：左侧标签、中间进度、右侧百分比。
 * 有了标签和数字，两根条才区分得开——只画色块的话就是"两条蓝线"。
 */
/**
 * 画一根条。
 * @param right   右列文字；NULL 表示按 resets_at 算倒计时
 * @param fill_override 非 0 时覆盖填充色（Context 按评级变色用）
 */
static void draw_limit_ex(uint16_t *fb, const text_canvas_t *tc, int y, const char *label,
                          float pct, bool cached, uint32_t age_sec, int64_t resets_at,
                          int64_t now_unix, const char *right, uint16_t fill_override)
{
    /* 额度栏整体压暗——高亮留给项目名和"正在运行"的状态 */
    const uint16_t label_col = COL_VERB;
    /* 缓存来源的行在标签后加 "*"，与实时值区分（陈旧天数见管理页） */
    char labelbuf[20];
    if (cached) {
        snprintf(labelbuf, sizeof(labelbuf), "%s*", label);
        label = labelbuf;
    }
    text_draw(tc, BAR_LABEL_CX - text_width(label, TEXT_SCALE) / 2, y + BAR_TEXT_DY,
              label, TEXT_SCALE, label_col);

    fill_rect(fb, BAR_X, y, BAR_W, BAR_H, COL_EMPTY);

    char buf[16];
    if (pct < 0.0f) {
        text_draw(tc, BAR_PCT_R - text_width("--", TEXT_SCALE), y + BAR_TEXT_DY, "--",
                  TEXT_SCALE, COL_VERB);
        return;
    }

    uint16_t fill = COL_FILL;
    if (pct >= 95.0f) fill = COL_CRIT;
    else if (pct >= 80.0f) fill = COL_WARN;
    if (cached) fill = COL_SUBTLE; /* 缓存来源用暗色，与实时值区分 */
    if (fill_override != 0) fill = fill_override;

    int w = (int)((pct / 100.0f) * (float)BAR_W);
    if (w < 3) w = 3;
    if (w > BAR_W) w = BAR_W;
    fill_rect(fb, BAR_X, y, w, BAR_H, fill);

    snprintf(buf, sizeof(buf), "%d%%", (int)pct);
    text_draw(tc, BAR_PCT_R - text_width(buf, TEXT_SCALE), y + BAR_TEXT_DY, buf,
              TEXT_SCALE, COL_NAME);

    /*
     * 重置时间列**永远只表示倒计时**，字号与左侧一致。
     * 之前这一格对缓存行显示的是"缓存年龄"、对实时行显示倒计时——
     * 同一列两种含义，必然被误读成"重置于 24 天前"。
     * 现在缓存行的陈旧程度改由标签后的 "*" 表示（见下），语义不再混淆。
     */
    (void)age_sec;
    if (right != NULL) {
        text_draw(tc, BAR_RESET_X, y + BAR_TEXT_DY, right, TEXT_SCALE, COL_RESET);
    } else {
        format_countdown(buf, sizeof(buf), resets_at, now_unix);
        text_draw(tc, BAR_RESET_X, y + BAR_TEXT_DY, buf[0] != '\0' ? buf : "--",
                  TEXT_SCALE, COL_RESET);
    }
}

/* 常用形态的薄包装 */
static void draw_limit(uint16_t *fb, const text_canvas_t *tc, int y, const char *label,
                       float pct, bool cached, uint32_t age_sec, int64_t resets_at,
                       int64_t now_unix)
{
    draw_limit_ex(fb, tc, y, label, pct, cached, age_sec, resets_at, now_unix, NULL, 0);
}

/**
 * 上下文占用的评级。
 * 分档按"离自动压缩还有多远"来定，而不是均分四段——
 * 75% 之前基本无感，90% 之后就该考虑收尾或开新会话了。
 */
static const char *context_rating(float pct, uint16_t *color)
{
    if (pct < 0.0f)  { *color = COL_EMPTY; return "--"; }
    if (pct < 50.0f) { *color = COL_OK;    return "roomy"; }
    if (pct < 75.0f) { *color = COL_FILL;  return "ok"; }
    if (pct < 90.0f) { *color = COL_WARN;  return "tight"; }
    *color = COL_CRIT; return "full";
}

/** 睡觉时头顶飘起的 ZZZ，三个字母错开相位、越飘越淡越小。 */
static void draw_zzz(const text_canvas_t *tc, int x, int y, uint32_t t)
{
    static const uint16_t FADE[3] = {0, 0, 0};
    (void)FADE;
    for (int i = 0; i < 3; i++) {
        const uint32_t period = 2400;
        const uint32_t phase = (t + (uint32_t)i * (period / 3)) % period;
        const float f = (float)phase / (float)period; /* 0..1 上升 */
        const int dx = x + (int)(f * 34.0f) + i * 4;
        const int dy = y - (int)(f * 46.0f);
        /* 越飘越暗：三档足够，避免逐像素混合 */
        const uint16_t col = f < 0.4f ? COL_DIM : (f < 0.75f ? COL_SUBTLE : COL_EMPTY);
        const int scale = f < 0.5f ? 3 : 2;
        text_draw(tc, dx, dy, "Z", scale, col);
    }
}

static void draw_chrome(uint16_t *fb, const text_canvas_t *tc, const session_t *focus,
                        clawd_state_t state, uint32_t t, int64_t now_unix)
{
    /* 顶部 subagent 圆点：实心=完成，空心=进行中 */
    fill_rect(fb, 0, DOTS_Y - DOT_R - 3, BSP_LCD_H_RES, DOT_R * 2 + 6, COL_BG);
    if (focus != NULL) {
        const int total = model_sub_total(focus);
        if (total > 0) {
            const int shown = total > 12 ? 12 : total;
            int x = BSP_LCD_H_RES / 2 - (shown - 1) * DOT_GAP / 2;
            int drawn = 0;
            for (int i = 0; i < MAX_SUBAGENTS && drawn < shown; i++) {
                if (!focus->subagents[i].used) continue;
                draw_dot(fb, x, DOTS_Y, DOT_R, COL_BODY, focus->subagents[i].done);
                x += DOT_GAP;
                drawn++;
            }
            if (total > shown) {
                char more[16];
                const int extra = total - shown > 99 ? 99 : total - shown;
                snprintf(more, sizeof(more), "+%d", extra);
                text_draw(tc, x + 4, DOTS_Y - 6, more, 1, COL_DIM);
            }
        }
    }

    /*
     * 项目名和状态词**分两行**。
     * 会话名长度不可控（实测有 "opinion-level-attribution-research" 这种 34 字符的），
     * 挤一行必然溢出屏幕；分行后各自还能独立降字号/截断。
     * 亮度也分层：名字是上下文（中灰），状态词更暗，把注意力留给底部的数据。
     */
    fill_rect(fb, 0, NAME_Y - 6, BSP_LCD_H_RES, VERB_Y - NAME_Y + TEXT_HEIGHT(TEXT_SCALE) + 12,
              COL_BG);
    if (focus != NULL) {
        /* 进行中的状态带省略号——"还在继续"是这三个点唯一要传达的意思；
         * done / idle 是终态，加了反而像卡住了。 */
        char verbbuf[40];
        const char *base = state == CLAWD_WORKING   ? verb_for(focus)
                           : state == CLAWD_WAITING ? "needs you"
                           : state == CLAWD_DONE    ? "done"
                                                    : "idle";
        const bool ongoing = (state == CLAWD_WORKING || state == CLAWD_WAITING);
        snprintf(verbbuf, sizeof(verbbuf), "%s%s", base, ongoing ? "..." : "");
        const char *verb = verbbuf;
        /* 只有"在跑"的状态才高亮；待机/睡觉压暗，避免和额度栏抢注意力 */
        const bool running = (state == CLAWD_WORKING || state == CLAWD_WAITING ||
                              state == CLAWD_DONE);
        draw_fit_center(tc, BSP_LCD_H_RES / 2, NAME_Y, focus->name, BSP_LCD_H_RES - 40,
                        TEXT_SCALE, COL_TEXT);
        draw_fit_center(tc, BSP_LCD_H_RES / 2, VERB_Y, verb, BSP_LCD_H_RES - 40,
                        TEXT_SCALE, running ? COL_TEXT : COL_VERB);
    } else {
        draw_fit_center(tc, BSP_LCD_H_RES / 2, NAME_Y, "no active session",
                        BSP_LCD_H_RES - 40, TEXT_SCALE, COL_VERB);
    }

    /* 底部四条等距 */
    fill_rect(fb, 0, BAR_Y0 - 8, BSP_LCD_H_RES, BSP_LCD_V_RES - BAR_Y0 + 8, COL_BG);
    draw_limit(fb, tc, BAR_Y0, "5h", s_model.limits.five_hour.pct, s_model.limits.cached,
               s_model.limits.age_sec, s_model.limits.five_hour.resets_at, now_unix);
    draw_limit(fb, tc, BAR_Y0 + BAR_DY, "Week", s_model.limits.seven_day.pct,
               s_model.limits.cached, s_model.limits.age_sec,
               s_model.limits.seven_day.resets_at, now_unix);

    /*
     * Context 放最后一行。它是**按会话**的，与上面两条账号级的不同类，
     * 但等距排更整齐；右列不写倒计时（上下文没有重置时间的概念），
     * 改写占用评级，并让色条跟着评级变色。
     *
     * 这里曾经还有一条 Fable。拿掉的原因见 docs/gotchas.md #11：
     * 按模型细分的额度**没有任何实时来源**，能拿到的只有 24 天前的缓存快照，
     * 而一个看起来实时、实际过期三周的数字比空着更糟。
     */
    uint16_t ctx_color = 0;
    const float ctx = focus != NULL ? focus->ctx_pct : -1.0f;
    const char *rating = context_rating(ctx, &ctx_color);
    draw_limit_ex(fb, tc, BAR_Y0 + BAR_DY * 2, "Context", ctx, false, 0, 0, now_unix,
                  rating, ctx_color);
}

/**
 * 界面静态部分的内容签名。变了才重画。
 *
 * 双缓冲下"每帧都擦掉重画"是频闪的主因之一：擦除和重画之间若发生扫描输出，
 * 就能看到中间态。只在内容真变时重画，且要连画两帧（两个缓冲各一次）。
 */
static uint32_t chrome_signature(const session_t *focus, clawd_state_t state)
{
    uint32_t h = 2166136261u;
    const uint32_t bits[] = {
        (uint32_t)state,
        focus ? (uint32_t)model_sub_total(focus) : 0u,
        focus ? (uint32_t)model_sub_done(focus) : 0u,
        focus ? (uint32_t)(int)(focus->ctx_pct * 10.0f) : 0u,
        (uint32_t)(int)(s_model.limits.five_hour.pct * 10.0f),
        (uint32_t)(int)(s_model.limits.seven_day.pct * 10.0f),
        (uint32_t)s_model.limits.cached,
    };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        h = (h ^ bits[i]) * 16777619u;
    }
    if (focus != NULL) {
        for (const char *p = focus->name; *p != '\0'; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        /* 状态词按轮次变，把轮次起点也算进去 */
        h = (h ^ (focus->status_since_ms / 1000u)) * 16777619u;
    }
    /* 倒计时按分钟变，纳入签名才会重画 */
    if (s_model.host_unix_sec > 0) {
        h = (h ^ (uint32_t)((s_model.host_unix_sec + now_ms() / 1000) / 60)) * 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    printf("\n\n######## Clawd Box ########\n");

    model_init(&s_model);
    s_model.limits.five_hour.pct = -1.0f;
    s_model.limits.seven_day.pct = -1.0f;

    ESP_ERROR_CHECK(bsp_board_init());
    ESP_ERROR_CHECK(bsp_display_init());

    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(bsp_display_get_framebuffer(&fb0, &fb1));
    uint16_t *const fb_main = (uint16_t *)fb0;
    (void)fb1;   /* num_fbs=1，只有一个缓冲 */
    clear_all(fb_main, COL_BG);

    /* scratch 按精灵包围盒的最大可能尺寸一次分配，稳态零 malloc */
    s_scratch_w = (int)((CLAWD_UNIT_W + 4) * SPRITE_SCALE) + 8;
    s_scratch_h = (int)((10 + 4) * SPRITE_SCALE) + 8;
    s_scratch = heap_caps_malloc((size_t)s_scratch_w * s_scratch_h * sizeof(uint16_t),
                                 MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_scratch == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    printf("  离屏缓冲 %dx%d (%u KB)\n", s_scratch_w, s_scratch_h,
           (unsigned)((size_t)s_scratch_w * s_scratch_h * 2 / 1024));
    ESP_ERROR_CHECK(bsp_display_backlight(true));

    ESP_ERROR_CHECK(link_start(&s_model));
    printf("  就绪。等待主机推送…（PSRAM 余 %u KB）\n\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    int focus_index = 0;
    uint32_t focus_since = now_ms();
    uint32_t state_since = now_ms();
    clawd_state_t last_state = CLAWD_SLEEPING;
    int last_active = -1;
    uint32_t report_at = now_ms();
    uint32_t last_sig = 0;
    int chrome_dirty = 1;   /* 单帧缓冲，画一次即可 */

    while (true) {
        const uint32_t t = now_ms();
        const int ring = model_ring_count(&s_model, t);

        /*
         * 抢占优先于轮播：**恰好一个**会话在干活时直接锁屏在它上面。
         * 那一刻你想盯着的就是它，没有什么可轮的。
         *
         * 抢占不成立时（没人在跑，或好几个同时在跑）才回到轮播，在
         * "在跑的 + 等输入的" 之间每 ROTATE_MS 换一个。空闲会话不进环。
         */
        session_t *focus = model_sole_busy(&s_model, t);
        if (focus != NULL) {
            focus_since = t; /* 抢占期间冻结计时，解除后从头开始轮 */
        } else {
            if (ring >= 2 && (uint32_t)(t - focus_since) > ROTATE_MS) {
                focus_index = (focus_index + 1) % ring;
                focus_since = t;
            }
            if (ring > 0 && focus_index >= ring) focus_index = 0;
            focus = ring > 0 ? model_ring_at(&s_model, focus_index, t) : NULL;
        }
        clawd_state_t state = focus != NULL ? model_clawd_state(focus, t) : CLAWD_SLEEPING;

        if (state == CLAWD_DONE && focus != NULL &&
            clawd_done_finished(t - focus->done_at_ms)) {
            focus->done_pending = false;
            state = model_clawd_state(focus, t);
        }
        if (state != last_state) {
            last_state = state;
            state_since = t;
        }

        uint16_t *fb = fb_main;
        const clawd_canvas_t canvas = {
            .pixels = fb, .width = BSP_LCD_H_RES, .height = BSP_LCD_V_RES};
        const text_canvas_t tc = {
            .pixels = fb, .width = BSP_LCD_H_RES, .height = BSP_LCD_V_RES};
        const clawd_draw_t params = {
            .center_x = BSP_LCD_H_RES / 2,
            .baseline_y = SPRITE_BASELINE_Y,
            .px_per_unit = SPRITE_SCALE,
            .state = state,
            .elapsed_ms = t - state_since,
            .body_color = COL_BODY,
            .eye_color = COL_EYE,
            .shadow_color = COL_BG,
        };

        clawd_rect_t box = clawd_bounds(&params);
        if (box.w > s_scratch_w) box.w = s_scratch_w;
        if (box.h > s_scratch_h) box.h = s_scratch_h;

        /* 1) 在 scratch 里擦干净并画好整块内容 */
        for (int i = 0; i < box.w * box.h; i++) s_scratch[i] = COL_BG;
        const clawd_canvas_t scratch_canvas = {
            .pixels = s_scratch, .width = box.w, .height = box.h};
        const text_canvas_t scratch_text = {
            .pixels = s_scratch, .width = box.w, .height = box.h};
        clawd_draw_t local = params;
        local.center_x = params.center_x - box.x;
        local.baseline_y = params.baseline_y - box.y;
        clawd_draw(&scratch_canvas, &local);
        if (state == CLAWD_SLEEPING) {
            draw_zzz(&scratch_text, local.center_x + 104,
                     local.baseline_y - (int)(9.0f * SPRITE_SCALE) + 14, t);
        }

        /* 2) 整块拷进帧缓冲——一次线性写入，不暴露中间态 */
        for (int row = 0; row < box.h; row++) {
            const int dst_y = box.y + row;
            if (dst_y < 0 || dst_y >= BSP_LCD_V_RES) continue;
            memcpy(fb + (size_t)dst_y * BSP_LCD_H_RES + box.x,
                   s_scratch + (size_t)row * box.w, (size_t)box.w * sizeof(uint16_t));
        }
        /* 静态部分只在内容变化时重画；两个缓冲各画一次 */
        const uint32_t sig = chrome_signature(focus, state);
        if (sig != last_sig) {
            last_sig = sig;
            chrome_dirty = 2;
        }
        if (chrome_dirty > 0) {
            const int64_t now_unix =
                s_model.host_unix_sec > 0
                    ? s_model.host_unix_sec + (int64_t)((t - s_model.host_sync_ms) / 1000u)
                    : 0;
            draw_chrome(fb, &tc, focus, state, t, now_unix);
            chrome_dirty--;
        }

        /* 单帧缓冲 + bounce：写入即显示，不需要 flush/交换 */

        if (ring != last_active || (uint32_t)(t - report_at) > 10000) {
            last_active = ring;
            report_at = t;
            printf("  在环=%d/%d 焦点=%s 状态=%d sub=%d/%d ctx=%.0f%% 5h=%.0f%% wk=%.0f%% 帧=%lums 丢帧=%lu\n",
                   ring, model_active_count(&s_model), focus ? focus->name : "-", (int)state,
                   focus ? model_sub_done(focus) : 0, focus ? model_sub_total(focus) : 0,
                   (double)(focus ? focus->ctx_pct : -1.0f),
                   (double)s_model.limits.five_hour.pct,
                   (double)s_model.limits.seven_day.pct,
                   (unsigned long)s_frame_ms, (unsigned long)link_dropped());
        }

        /*
         * **必须至少让出一个 tick。**
         * 原来超预算时调 taskYIELD()，但它只在同优先级之间轮转，
         * 不会调度到优先级更低的 IDLE——精灵放大后单帧超出预算，
         * 于是渲染任务再也不阻塞，IDLE0 饿死，任务看门狗每 5 秒报一次。
         */
        const int64_t spent = (int64_t)now_ms() - (int64_t)t;
        const TickType_t rest = spent < FRAME_MS ? pdMS_TO_TICKS(FRAME_MS - spent) : 0;
        vTaskDelay(rest > 0 ? rest : 1);
        s_frame_ms = (uint32_t)spent;
    }
}
