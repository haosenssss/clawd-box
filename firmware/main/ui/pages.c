#include "pages.h"

#include "theme.h"
#include "bsp/board_config.h"
#include <stdio.h>
#include <string.h>

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

void ui_clear_all(uint16_t *fb, uint16_t c)
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
                          float pct, int64_t resets_at, int64_t now_unix, const char *right,
                          uint16_t fill_override)
{
    /* 额度栏整体压暗——高亮留给项目名和"正在运行"的状态 */
    text_draw(tc, BAR_LABEL_CX - text_width(label, TEXT_SCALE) / 2, y + BAR_TEXT_DY,
              label, TEXT_SCALE, COL_VERB);

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
    if (fill_override != 0) fill = fill_override;

    int w = (int)((pct / 100.0f) * (float)BAR_W);
    if (w < 3) w = 3;
    if (w > BAR_W) w = BAR_W;
    fill_rect(fb, BAR_X, y, w, BAR_H, fill);

    snprintf(buf, sizeof(buf), "%d%%", (int)pct);
    text_draw(tc, BAR_PCT_R - text_width(buf, TEXT_SCALE), y + BAR_TEXT_DY, buf,
              TEXT_SCALE, COL_NAME);

    /*
     * 最右一列**永远只表示倒计时**，拿不到就是 "--"。
     * 不用 "*" 之类的角标去标注数据新鲜度，也不拿假定值占位——
     * 一个不确定的数字摆在那里，比一个诚实的 "--" 更容易被当真。
     */
    if (right != NULL) {
        text_draw(tc, BAR_RIGHT_EDGE - text_width(right, TEXT_SCALE), y + BAR_TEXT_DY, right,
                  TEXT_SCALE, COL_RESET);
    } else {
        format_countdown(buf, sizeof(buf), resets_at, now_unix);
        const char *txt = buf[0] != '\0' ? buf : "--";
        text_draw(tc, BAR_RIGHT_EDGE - text_width(txt, TEXT_SCALE), y + BAR_TEXT_DY, txt,
                  TEXT_SCALE, COL_RESET);
    }
}

/* 常用形态的薄包装 */
static void draw_limit(uint16_t *fb, const text_canvas_t *tc, int y, const char *label,
                       float pct, int64_t resets_at, int64_t now_unix)
{
    draw_limit_ex(fb, tc, y, label, pct, resets_at, now_unix, NULL, 0);
}

/**
 * 上下文占用的评级。
 * 分档按"离自动压缩还有多远"来定，而不是均分四段——
 * 75% 之前基本无感，90% 之后就该考虑收尾或开新会话了。
 */
static const char *context_rating(float pct, uint16_t *color)
{
    /*
     * **最多三个字符。** 这一列右边界离屏幕边缘只有 ~40px，
     * "roomy" 这种五个字母的词会直接溢出屏幕右侧。
     * 严重程度由色条颜色承担，文字只报占用档位就够了。
     */
    if (pct < 0.0f)  { *color = COL_EMPTY; return "--"; }
    if (pct < 50.0f) { *color = COL_OK;    return "ok"; }
    if (pct < 75.0f) { *color = COL_FILL;  return "mid"; }
    if (pct < 90.0f) { *color = COL_WARN;  return "hi"; }
    *color = COL_CRIT; return "max";
}

/** 睡觉时头顶飘起的 ZZZ，三个字母错开相位、越飘越淡越小。 */
void draw_zzz(const text_canvas_t *tc, int x, int y, uint32_t t)
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

void session_page_draw(uint16_t *fb, const text_canvas_t *tc, const model_t *m,
                       const session_t *focus, clawd_state_t state, uint32_t now_ms_,
                       int64_t now_unix)
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
    draw_limit(fb, tc, BAR_Y0, "5h", m->limits.five_hour.pct,
               m->limits.five_hour.resets_at, now_unix);
    draw_limit(fb, tc, BAR_Y0 + BAR_DY, "Week", m->limits.seven_day.pct,
               m->limits.seven_day.resets_at, now_unix);

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
    draw_limit_ex(fb, tc, BAR_Y0 + BAR_DY * 2, "Context", ctx, 0, now_unix, rating,
                  ctx_color);
}

/**
 * 界面静态部分的内容签名。变了才重画。
 *
 * 双缓冲下"每帧都擦掉重画"是频闪的主因之一：擦除和重画之间若发生扫描输出，
 * 就能看到中间态。只在内容真变时重画，且要连画两帧（两个缓冲各一次）。
 */
uint32_t session_page_signature(const model_t *m, const session_t *focus,
                                clawd_state_t state, uint32_t now_ms_)
{
    uint32_t h = 2166136261u;
    const uint32_t bits[] = {
        (uint32_t)state,
        focus ? (uint32_t)model_sub_total(focus) : 0u,
        focus ? (uint32_t)model_sub_done(focus) : 0u,
        focus ? (uint32_t)(int)(focus->ctx_pct * 10.0f) : 0u,
        (uint32_t)(int)(m->limits.five_hour.pct * 10.0f),
        (uint32_t)(int)(m->limits.seven_day.pct * 10.0f),
        (uint32_t)m->limits.cached,
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
    if (m->host_unix_sec > 0) {
        h = (h ^ (uint32_t)((m->host_unix_sec + now_ms_ / 1000) / 60)) * 16777619u;
    }
    return h;
}

/* ------------------------------------------------------------------ *
 * 管理页
 *
 * 与会话页共用底部三条额度栏（同样的 y、同样的宽），所以左右滑动时
 * 下半屏是**不动的**——只有上半屏在换。这比整屏切换安静得多，
 * 也让"限额是常驻信息"这件事在视觉上成立。
 * ------------------------------------------------------------------ */

#define ADMIN_TITLE_Y 34
#define ADMIN_ROW_Y0 84
#define ADMIN_ROW_DY 38
#define ADMIN_ROW_MAX 6
#define ADMIN_DOT_X 32
#define ADMIN_NAME_X 54

static const char *status_word(const session_t *s)
{
    switch (s->status) {
        case SESS_BUSY: return "working";
        case SESS_WAITING: return "needs you";
        case SESS_IDLE:
        default: return "idle";
    }
}

static uint16_t status_color(const session_t *s)
{
    switch (s->status) {
        case SESS_BUSY: return COL_BODY;
        case SESS_WAITING: return COL_WARN;
        case SESS_IDLE:
        default: return COL_SUBTLE;
    }
}

void admin_page_draw(uint16_t *fb, const text_canvas_t *tc, const model_t *m,
                     uint32_t now_ms_, int64_t now_unix)
{
    (void)now_ms_;
    fill_rect(fb, 0, 0, BSP_LCD_H_RES, BAR_Y0 - 8, COL_BG);

    const int total = model_active_count(m);
    char head[32];
    snprintf(head, sizeof(head), "SESSIONS  %d", total);
    text_draw(tc, ADMIN_DOT_X, ADMIN_TITLE_Y, head, TEXT_SCALE, COL_NAME);

    /* 标题下一条细分隔线——管理页要有"总控台"的份量，不能和会话页糊在一起 */
    fill_rect(fb, ADMIN_DOT_X, ADMIN_TITLE_Y + 24, BSP_LCD_H_RES - ADMIN_DOT_X * 2, 1,
              COL_SUBTLE);

    if (total == 0) {
        text_draw(tc, ADMIN_DOT_X, ADMIN_ROW_Y0, "no sessions", TEXT_SCALE, COL_VERB);
        return;
    }

    const int rows = total < ADMIN_ROW_MAX ? total : ADMIN_ROW_MAX;
    for (int i = 0; i < rows; i++) {
        const session_t *s = model_at((model_t *)m, i);
        if (s == NULL) continue;
        const int y = ADMIN_ROW_Y0 + i * ADMIN_ROW_DY;
        const uint16_t col = status_color(s);

        /* 实心点=在跑或在等，空心=空闲。和 subagent 圆点同一套语汇。 */
        draw_dot(fb, ADMIN_DOT_X, y + 6, 5, col, s->status != SESS_IDLE);

        /* 状态词右对齐，名字占左边剩下的宽度并按需截断 */
        const char *word = status_word(s);
        const int word_w = text_width(word, TEXT_SCALE);
        const int word_x = BSP_LCD_H_RES - ADMIN_DOT_X - word_w;
        text_draw(tc, word_x, y, word, TEXT_SCALE, col);

        char name[SESSION_NAME_LEN];
        snprintf(name, sizeof(name), "%s", s->name[0] != '\0' ? s->name : "(unnamed)");
        const int avail = word_x - ADMIN_NAME_X - 12;
        int scale = TEXT_SCALE;
        while (scale > 1 && text_width(name, scale) > avail) scale--;
        /* 缩到最小还放不下就截断，宁可少几个字也不要压到状态词上 */
        while (name[0] != '\0' && text_width(name, scale) > avail) {
            name[strlen(name) - 1] = '\0';
        }
        text_draw(tc, ADMIN_NAME_X, y + (TEXT_SCALE - scale) * 3, name, scale,
                  s->status == SESS_IDLE ? COL_VERB : COL_TEXT);
    }

    if (total > ADMIN_ROW_MAX) {
        char more[24];
        snprintf(more, sizeof(more), "+%d more", total - ADMIN_ROW_MAX);
        text_draw(tc, ADMIN_NAME_X, ADMIN_ROW_Y0 + ADMIN_ROW_MAX * ADMIN_ROW_DY, more,
                  TEXT_SCALE, COL_VERB);
    }
    (void)now_unix;
}

uint32_t admin_page_signature(const model_t *m, int64_t now_unix)
{
    uint32_t h = 0x9E3779B9u;
    h = (h ^ (uint32_t)model_active_count(m)) * 16777619u;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        const session_t *s = model_at((model_t *)m, i);
        if (s == NULL) break;
        h = (h ^ (uint32_t)s->status) * 16777619u;
        for (const char *p = s->name; *p != '\0'; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    }
    h = (h ^ (uint32_t)(now_unix / 60)) * 16777619u;
    return h;
}
