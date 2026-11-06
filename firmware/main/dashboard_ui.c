#include "dashboard_ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "app_types.h"
#include "board.h"
#include "lc_badge.h"
#include "lc_task.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#if CONFIG_CKB_DEBUG_SCREENSHOTS
#include "freertos/queue.h"
#include "screenshot_stream.h"
#endif

#define COLOR_BG 0x081014
#define COLOR_PANEL 0x101c22
#define COLOR_GRID 0x203039
#define COLOR_TEXT 0xe4f2f3
#define COLOR_MUTED 0x8aa2a8
#define COLOR_GREEN 0x00d68f
#define COLOR_BLUE 0x59b9ff
#define COLOR_AMBER 0xffb547
#define COLOR_RED 0xff5d6c

static const char *TAG = "ui";
static const char *PAGE_NAMES[7] = {"Overview", "Blocks", "Transactions", "Network", "Economics", "Capacity", "Client"};
static const char *NAV_NAMES[7] = {"HOME", "BLK", "TX", "NET", "ECON", "CAP", "LC"};
static const uint64_t WINDOWS_MS[] = {3600000ULL, 21600000ULL, 86400000ULL, 604800000ULL, 2592000000ULL};
static const char *WINDOW_NAMES[] = {"1H", "6H", "24H", "7D", "30D"};

#define PAGE_HEIGHT 336
#define CHART_X 60
#define CHART_Y 80
#define CHART_W 404
#define CHART_H 168
#define CHART_RANGE 1000
/* Plotted points per chart: ~7 px per line segment, ~13 px per bar. */
#define CHART_LINE_POINTS 60
#define CHART_BAR_POINTS 30

typedef struct {
    lv_obj_t *root;
    lv_obj_t *primary;
    lv_obj_t *secondary;
    lv_obj_t *detail;
    lv_obj_t *chart;
    lv_obj_t *coverage;
    lv_obj_t *range;
    lv_obj_t *axis[3]; /* y-axis labels: max, mid, min */
    lv_chart_series_t *series;
    double chart_min;
    double chart_max;
} page_t;

static QueueHandle_t s_snapshots;
static QueueHandle_t s_controls;
static SemaphoreHandle_t s_lvgl_lock;
static metric_store_t *s_store;
static dashboard_snapshot_t s_snapshot;
static page_t s_pages[7];
static lv_obj_t *s_nav[7];
static lv_obj_t *s_status;
static lv_obj_t *s_age;
static lv_obj_t *s_source;
static lv_obj_t *s_title;
static uint8_t s_page;
static uint8_t s_window;
static bool s_mode_known;
static bool s_demo_seeded;
static bool s_ui_started;

static void maybe_show_scan_results(void);
static void refresh_ui(void);

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static SemaphoreHandle_t s_vsync;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

/* DIRECT mode with the panel's two framebuffers: LVGL calls this once per
 * dirty area, and `pixels` is always the start of the buffer it drew into.
 * Handing that pointer to the RGB driver schedules a buffer swap, so it must
 * happen only after the LAST area, and LVGL may only reuse the old buffer
 * once the panel has actually switched (next vsync). Swapping per area and
 * releasing immediately showed half-drawn frames: visible flicker. */
static void flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }
    xSemaphoreTake(s_vsync, 0); /* Drop a stale vsync from an earlier frame. */
    esp_err_t err = esp_lcd_panel_draw_bitmap(board_panel(), 0, 0, BOARD_HRES, BOARD_VRES, pixels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(err));
    } else if (xSemaphoreTake(s_vsync, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "No vsync within 100 ms");
    }
    lv_display_flush_ready(display);
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;
    esp_lcd_touch_handle_t touch = board_touch();
    if (!touch || esp_lcd_touch_read_data(touch) != ESP_OK) {
        return;
    }
    esp_lcd_touch_point_data_t point;
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(touch, &point, &count, 1) == ESP_OK && count > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                       int width, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, color, 0);
    return object;
}

/* LVGL's built-in printf is compiled without float support
 * (CONFIG_LV_USE_FLOAT off), so "%.1f" rendered as a bare "f". All label
 * formatting goes through libc vsnprintf instead. */
static void set_text_fmt(lv_obj_t *object, const char *format, ...)
{
    char text[128];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    lv_label_set_text(object, text);
}

/* One line, truncated with "..." instead of wrapping into the next widget. */
static void single_line(lv_obj_t *object, const lv_font_t *font)
{
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_obj_set_height(object, lv_font_get_line_height(font));
}

/* Content-sized label centred in its button. A fixed-width label is
 * left-aligned inside that width, which pushed button text off centre. */
static lv_obj_t *button_label(lv_obj_t *button, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *object = lv_label_create(button);
    lv_label_set_text(object, text);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, color, 0);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    /* Button captions never wrap; clip rather than break onto a second line. */
    lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
    lv_obj_center(object);
    return object;
}

/* Compact axis/summary numbers: 1.25P, 39.8k, 9.87. */
static void format_compact(char *out, size_t size, double value)
{
    static const char *SUFFIX[] = {"", "k", "M", "G", "T", "P", "E"};
    size_t unit = 0;
    double magnitude = fabs(value);
    while (magnitude >= 1000.0 && unit + 1 < sizeof(SUFFIX) / sizeof(SUFFIX[0])) {
        magnitude /= 1000.0;
        value /= 1000.0;
        unit++;
    }
    const char *format = magnitude >= 100.0 ? "%.0f%s" : (magnitude >= 10.0 ? "%.1f%s" : "%.2f%s");
    snprintf(out, size, format, value, SUFFIX[unit]);
}

/* Signed compact offset: "+21.0k", "-610", "+0". */
static void format_offset(char *out, size_t size, double value)
{
    char body[15]; /* + sign + NUL fits the 16-byte axis labels */
    format_compact(body, sizeof(body), fabs(value));
    snprintf(out, size, "%s%s", value < 0.0 ? "-" : "+", body);
}

static void style_button(lv_obj_t *button, bool active)
{
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(active ? COLOR_GRID : COLOR_BG), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, active ? 2 : 0, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_text_color(button, lv_color_hex(active ? COLOR_GREEN : COLOR_MUTED), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    /* The default theme pads buttons ~13-20 px per side at 130 DPI, leaving a
     * 64 px nav button too narrow for "HOME", which wrapped onto two lines. */
    lv_obj_set_style_pad_hor(button, 2, 0);
}

static void show_page(uint8_t page)
{
    if (page >= 7) {
        return;
    }
    s_page = page;
    for (uint8_t i = 0; i < 7; ++i) {
        if (i == page) {
            lv_obj_remove_flag(s_pages[i].root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i].root, LV_OBJ_FLAG_HIDDEN);
        }
        style_button(s_nav[i], i == page);
    }
    lv_label_set_text(s_title, PAGE_NAMES[page]);
}

static void nav_event(lv_event_t *event)
{
    show_page((uint8_t)(uintptr_t)lv_event_get_user_data(event));
}

static void gesture_event(lv_event_t *event)
{
    (void)event;
    lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    if (direction == LV_DIR_LEFT && s_page < 6) {
        show_page(s_page + 1);
    } else if (direction == LV_DIR_RIGHT && s_page > 0) {
        show_page(s_page - 1);
    }
}

static void range_event(lv_event_t *event)
{
    (void)event;
    s_window = (s_window + 1) % 5;
    for (size_t i = 0; i < 7; ++i) {
        lv_label_set_text(s_pages[i].range, WINDOW_NAMES[s_window]);
    }
    refresh_ui();
}

static void chart_event(lv_event_t *event)
{
    page_t *page = lv_event_get_user_data(event);
    if (!page->chart || lv_event_get_code(event) != LV_EVENT_PRESSING) {
        return;
    }
    int32_t point = lv_chart_get_pressed_point(page->chart);
    if (point != LV_CHART_POINT_NONE && page->chart_max > page->chart_min) {
        lv_coord_t scaled = lv_chart_get_y_array(page->chart, page->series)[point];
        double value = page->chart_min + (page->chart_max - page->chart_min) * scaled / CHART_RANGE;
        char text[16];
        if (page == &s_pages[5]) {
            format_offset(text, sizeof(text), value); /* capacity plots change */
        } else {
            format_compact(text, sizeof(text), value);
        }
        set_text_fmt(page->coverage, "Point %ld of %lu: %s", (long)point + 1,
                     (unsigned long)lv_chart_get_point_count(page->chart), text);
    }
}

static void add_point(metric_id_t id, double value, uint64_t timestamp_ms,
                      uint32_t height, metric_source_t source, metric_quality_t quality)
{
    if (!s_store || !isfinite(value)) {
        return;
    }
    metric_point_t point = {
        .value = value,
        .timestamp_ms = timestamp_ms,
        .block_height = height,
        .source = source,
        .quality = quality,
        .valid = 1,
    };
    metric_ring_add(&s_store->series[id], &point);
}

static void seed_demo(void)
{
    if (s_demo_seeded) {
        return;
    }
    for (uint32_t i = 0; i < 120; ++i) {
        uint64_t timestamp = 1793650000000ULL + i * 10000ULL;
        double tx = 3.0 + (i % 7) + 2.0 * sin(i / 4.0);
        add_point(METRIC_TX_PER_BLOCK, tx, timestamp, 17000000 + i, SOURCE_DEMO, QUALITY_LIVE);
        add_point(METRIC_BLOCK_INTERVAL, 10.0 + (i % 5), timestamp, 17000000 + i, SOURCE_DERIVED, QUALITY_LIVE);
        add_point(METRIC_DIFFICULTY, 1.15e15 + i * 1.0e12, timestamp, 17000000 + i, SOURCE_DEMO, QUALITY_LIVE);
        add_point(METRIC_OCCUPIED_CKB, 5.1841e9 + i * 61.0 + 400.0 * sin(i / 9.0), timestamp, 17000000 + i,
                  SOURCE_DEMO, QUALITY_LIVE);
    }
    s_demo_seeded = true;
}

static uint32_t s_latest_scan_seq;
static bool s_latest_scan_ok;
static char s_scanned_ssids[SCAN_MAX_RESULTS][33];
static int8_t s_scanned_rssi[SCAN_MAX_RESULTS];
static uint8_t s_scanned_count;

static void cache_scan_results(const dashboard_snapshot_t *snapshot)
{
    if (snapshot->scan_seq == s_latest_scan_seq) {
        return;
    }
    s_latest_scan_seq = snapshot->scan_seq;
    s_latest_scan_ok = snapshot->scan_ok;
    s_scanned_count = snapshot->scan_count > SCAN_MAX_RESULTS ? SCAN_MAX_RESULTS : snapshot->scan_count;
    memcpy(s_scanned_ssids, snapshot->scan_ssids, sizeof(s_scanned_ssids));
    memcpy(s_scanned_rssi, snapshot->scan_rssi, sizeof(s_scanned_rssi));
}

static void ingest_snapshot(const dashboard_snapshot_t *snapshot)
{
    cache_scan_results(snapshot);
    if (!s_store) {
        return;
    }
    if (!s_mode_known || s_snapshot.demo != snapshot->demo || snapshot->reorg) {
        metric_store_clear(s_store);
        s_demo_seeded = false;
        s_mode_known = true;
    }
    if (snapshot->demo) {
        seed_demo();
    }
    if (snapshot->rpc_ok && !snapshot->stale) {
        for (int32_t i = (int32_t)snapshot->block_count - 1; i >= 0; --i) {
            const rpc_block_summary_t *block = &snapshot->blocks[i];
            if (!block->valid || block->height > UINT32_MAX) {
                continue;
            }
            add_point(METRIC_TX_PER_BLOCK, block->tx_count, block->timestamp_ms,
                      block->height, snapshot->source, QUALITY_LIVE);
            if (i + 1 < (int32_t)snapshot->block_count) {
                const rpc_block_summary_t *older = &snapshot->blocks[i + 1];
                if (older->valid && block->timestamp_ms >= older->timestamp_ms) {
                    add_point(METRIC_BLOCK_INTERVAL,
                              (double)(block->timestamp_ms - older->timestamp_ms) / 1000.0,
                              block->timestamp_ms, block->height, SOURCE_DERIVED, QUALITY_LIVE);
                }
            }
            if (block->has_dao) {
                add_point(METRIC_OCCUPIED_CKB, (double)block->dao.u / 1e8,
                          block->timestamp_ms, block->height, snapshot->source, QUALITY_LIVE);
            }
            if (block->compact_target) {
                add_point(METRIC_DIFFICULTY, ckb_compact_target_difficulty(block->compact_target),
                          block->timestamp_ms, block->height, SOURCE_DERIVED, QUALITY_LIVE);
            }
        }
        uint64_t height = snapshot->tip_height > UINT32_MAX ? UINT32_MAX : snapshot->tip_height;
        if (snapshot->has_pool) {
            add_point(METRIC_POOL_PENDING, snapshot->pool_pending, snapshot->tip_timestamp_ms,
                      height, snapshot->source, QUALITY_LIVE);
            add_point(METRIC_POOL_BYTES, snapshot->pool_bytes, snapshot->tip_timestamp_ms,
                      height, snapshot->source, QUALITY_LIVE);
        }
        if (snapshot->has_fee) {
            add_point(METRIC_FEE_RATE, snapshot->fee_rate, snapshot->tip_timestamp_ms,
                      height, snapshot->source, QUALITY_LIVE);
        }
        if (snapshot->has_peers) {
            add_point(METRIC_PEERS, snapshot->peers, snapshot->tip_timestamp_ms,
                      height, snapshot->source, QUALITY_LIVE);
        }
        if (snapshot->has_epoch) {
            add_point(METRIC_EPOCH_PROGRESS,
                      ckb_epoch_progress(snapshot->tip_height, snapshot->epoch_start, snapshot->epoch_length) * 100.0,
                      snapshot->tip_timestamp_ms, height, SOURCE_DERIVED, QUALITY_LIVE);
        }
    }
    s_snapshot = *snapshot;
}

static const metric_ring_t *page_ring(uint8_t page)
{
    if (!s_store) {
        return NULL;
    }
    switch (page) {
        case 0:
        case 1: return &s_store->series[METRIC_BLOCK_INTERVAL];
        case 2: return &s_store->series[METRIC_TX_PER_BLOCK];
        case 3: return &s_store->series[METRIC_POOL_PENDING];
        case 4: return &s_store->series[METRIC_DIFFICULTY];
        case 5: return &s_store->series[METRIC_OCCUPIED_CKB];
        default: return NULL;
    }
}

/* Scratch for chart/summary reads: 240 points is ~7.7 KiB, too big for the
 * UI task stack. Only the UI task touches these. */
static metric_point_t s_view[METRIC_VIEW_CAP];
static double s_view_values[METRIC_VIEW_CAP];
static double s_plot[CHART_LINE_POINTS];

static uint64_t ring_newest_ms(const metric_ring_t *ring)
{
    uint64_t newest = 0;
    for (uint16_t i = 0; i < ring->count; ++i) {
        if (ring->points[i].valid && ring->points[i].timestamp_ms > newest) {
            newest = ring->points[i].timestamp_ms;
        }
    }
    return newest;
}

/* Points inside the selected time window, oldest first. */
static size_t read_window(const metric_ring_t *ring, uint64_t *newest_out)
{
    uint64_t newest = ring_newest_ms(ring);
    if (newest_out) {
        *newest_out = newest;
    }
    uint64_t window = WINDOWS_MS[s_window];
    return metric_ring_read(ring, newest >= window ? newest - window : 0, s_view, METRIC_VIEW_CAP);
}

static bool window_mean(const metric_ring_t *ring, double *mean, size_t *count)
{
    size_t n = read_window(ring, NULL);
    if (n == 0) {
        return false;
    }
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += s_view[i].value;
    }
    *mean = sum / (double)n;
    *count = n;
    return true;
}

static void set_axis(page_t *page, const char *top, const char *mid, const char *bottom)
{
    lv_label_set_text(page->axis[0], top);
    lv_label_set_text(page->axis[1], mid);
    lv_label_set_text(page->axis[2], bottom);
}

static void update_chart(page_t *page, uint8_t index)
{
    const metric_ring_t *ring = page_ring(index);
    if (!ring) {
        lv_obj_add_flag(page->chart, LV_OBJ_FLAG_HIDDEN);
        set_axis(page, "", "", "");
        return;
    }
    lv_obj_remove_flag(page->chart, LV_OBJ_FLAG_HIDDEN);
    uint64_t newest = 0;
    size_t count = read_window(ring, &newest);
    if (count == 0) {
        lv_chart_set_point_count(page->chart, 2);
        lv_chart_set_all_value(page->chart, page->series, LV_CHART_POINT_NONE);
        set_axis(page, "--", "", "--");
        lv_label_set_text(page->coverage, "No samples in this window; gaps are not zero");
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        s_view_values[i] = s_view[i].value;
    }
    size_t cap = lv_chart_get_type(page->chart) == LV_CHART_TYPE_BAR ? CHART_BAR_POINTS : CHART_LINE_POINTS;
    size_t plotted = ckb_downsample_mean(s_view_values, count, s_plot, cap);
    if (plotted == 1) {
        s_plot[1] = s_plot[0]; /* a line needs two points */
        plotted = 2;
    }
    if (index == 5) {
        /* Occupied capacity moves a few ppm per hour on a ~5.18G base, so
         * absolute labels would all read "5.18G". Plot the change since the
         * window's oldest plotted block; the headline has the total. */
        double base = s_plot[0];
        for (size_t i = 0; i < plotted; ++i) {
            s_plot[i] -= base;
        }
    }
    double min;
    double max;
    ckb_chart_bounds(s_plot, plotted, lv_chart_get_type(page->chart) == LV_CHART_TYPE_BAR, &min, &max);
    page->chart_min = min;
    page->chart_max = max;
    lv_chart_set_point_count(page->chart, (uint32_t)plotted);
    for (size_t i = 0; i < plotted; ++i) {
        int32_t scaled = (int32_t)lround((s_plot[i] - min) * CHART_RANGE / (max - min));
        lv_chart_set_value_by_id(page->chart, page->series, (uint32_t)i, scaled);
    }
    lv_chart_refresh(page->chart);
    char top[16];
    char mid[16];
    char bottom[16];
    if (index == 5) {
        format_offset(top, sizeof(top), max);
        format_offset(mid, sizeof(mid), (max + min) / 2.0);
        format_offset(bottom, sizeof(bottom), min);
    } else {
        format_compact(top, sizeof(top), max);
        format_compact(mid, sizeof(mid), (max + min) / 2.0);
        format_compact(bottom, sizeof(bottom), min);
    }
    set_axis(page, top, mid, bottom);
    double coverage = metric_ring_coverage(ring, newest, WINDOWS_MS[s_window]) * 100.0;
    set_text_fmt(page->coverage, "%s: %u samples, %u plotted | %.0f%% of window observed",
                 WINDOW_NAMES[s_window], (unsigned)count, (unsigned)plotted, coverage);
}

static void format_integer(char *out, size_t size, uint64_t value)
{
    ckb_format_grouped_u64(out, size, value);
}

static bool latest_point(metric_id_t id, metric_point_t *out)
{
    const metric_ring_t *ring = &s_store->series[id];
    if (!ring->count) {
        return false;
    }
    *out = ring->points[(ring->next + METRIC_HISTORY_CAP - 1) % METRIC_HISTORY_CAP];
    return out->valid;
}

static const char *lc_state_text(uint8_t state)
{
    switch ((lc_state_t)state) {
    case LC_STATE_WAITING: return "waiting for Wi-Fi";
    case LC_STATE_CONNECTING: return "connecting to peer";
    case LC_STATE_ANCHORING: return "proving from checkpoint";
    case LC_STATE_PROVEN: return "following the proven tip";
    case LC_STATE_RETRYING: return "reconnecting";
    case LC_STATE_DISABLED:
    default: return "disabled";
    }
}

static void update_light_client_page(page_t *p)
{
    lc_badge_t badge = (lc_badge_t)s_snapshot.lc_badge;
    uint32_t color = badge == LC_BADGE_PROVEN ? COLOR_GREEN
                   : badge == LC_BADGE_RPC_MISMATCH ? COLOR_RED
                   : badge == LC_BADGE_ANCHORING ? COLOR_AMBER : COLOR_MUTED;
    lv_label_set_text(p->primary, badge == LC_BADGE_OFF ? "OFF" : lc_badge_text(badge));
    lv_obj_set_style_text_color(p->primary, lv_color_hex(color), 0);
    if (s_snapshot.lc_has_tip) {
        set_text_fmt(p->secondary, "Proven tip %llu (%lus ago), %s",
                     (unsigned long long)s_snapshot.lc_height, (unsigned long)s_snapshot.lc_age_s,
                     lc_state_text(s_snapshot.lc_state));
    } else {
        set_text_fmt(p->secondary, "No tip proven yet, %s", lc_state_text(s_snapshot.lc_state));
    }
    const char *check = s_snapshot.lc_check == LC_CHECK_MATCH ? "RPC agrees"
                      : s_snapshot.lc_check == LC_CHECK_DIFFER ? "RPC DIFFERS"
                      : "RPC not compared";
    set_text_fmt(p->detail, "%s | hash %.18s...", check,
                 s_snapshot.lc_has_tip ? s_snapshot.lc_hash : "--");
#if CONFIG_CKB_LC_ENABLE
    set_text_fmt(p->coverage, "P2P %s:%d | checkpoint %llu | session %lu", CONFIG_CKB_LC_HOST,
                 CONFIG_CKB_LC_PORT, (unsigned long long)s_snapshot.lc_checkpoint,
                 (unsigned long)s_snapshot.lc_sessions);
#else
    lv_label_set_text(p->coverage, "Light client not compiled in (CONFIG_CKB_LC_ENABLE)");
#endif
}

/* Total occupied capacity from the newest header's dao field (u), with its
 * share of issued capacity (c) and the NervosDAO accumulated rate (ar). */
static void update_capacity_page(page_t *p)
{
    char number[40];
    char issued[16];
    const rpc_block_summary_t *tip = s_snapshot.block_count ? &s_snapshot.blocks[0] : NULL;
    metric_point_t latest;
    if (!s_snapshot.demo && tip && tip->valid && tip->has_dao && tip->dao.c > 0) {
        format_integer(number, sizeof(number), tip->dao.u / 100000000ULL);
        set_text_fmt(p->primary, "%s CKB", number);
        format_compact(issued, sizeof(issued), (double)tip->dao.c / 1e8);
        set_text_fmt(p->secondary, "%.2f%% of %s CKB issued | DAO rate %.4fx",
                     (double)tip->dao.u / (double)tip->dao.c * 100.0, issued,
                     (double)tip->dao.ar / 1e16);
    } else if (s_snapshot.demo && latest_point(METRIC_OCCUPIED_CKB, &latest)) {
        format_integer(number, sizeof(number), (uint64_t)latest.value);
        set_text_fmt(p->primary, "%s CKB", number);
        lv_label_set_text(p->secondary, "Demo fixture, not chain data");
    } else {
        lv_label_set_text(p->primary, "--");
        lv_label_set_text(p->secondary, "Waiting for a block header with a dao field");
    }
    lv_label_set_text(p->detail, "Occupied CKB from header dao (U). Axis: change in window. RPC data.");
}

static void update_page_text(uint8_t index)
{
    page_t *p = &s_pages[index];
    char number[40];
    char extra[24];
    metric_point_t latest;
    double mean = 0.0;
    size_t samples = 0;
    switch (index) {
        case 0:
            if (s_snapshot.has_tip) {
                format_integer(number, sizeof(number), s_snapshot.tip_height);
                set_text_fmt(p->primary, "#%s", number);
                if (s_snapshot.has_epoch) {
                    double progress = ckb_epoch_progress(s_snapshot.tip_height, s_snapshot.epoch_start,
                                                         s_snapshot.epoch_length) * 100.0;
                    set_text_fmt(p->secondary, "Epoch %llu | %.1f%% complete | %u tx in tip",
                                 (unsigned long long)s_snapshot.epoch_number, progress,
                                 s_snapshot.block_count ? (unsigned)s_snapshot.blocks[0].tx_count : 0U);
                } else {
                    lv_label_set_text(p->secondary, "Epoch unavailable from this endpoint");
                }
            } else {
                lv_label_set_text(p->primary, "--");
                lv_label_set_text(p->secondary, "Waiting for a trusted RPC response");
            }
            /* The values are RPC data either way; the light client only says
             * whether the RPC's chain is the one it proved. */
            if (!s_snapshot.demo && s_snapshot.lc_badge == LC_BADGE_PROVEN) {
                lv_label_set_text(p->detail, "Chart: block interval (s). RPC data; its chain matches the light-client proof.");
            } else if (!s_snapshot.demo && s_snapshot.lc_badge == LC_BADGE_RPC_MISMATCH) {
                lv_label_set_text(p->detail, "Chart: block interval (s). RPC data DISAGREES with the light-client proof.");
            } else {
                lv_label_set_text(p->detail, "Chart: block interval (s). Tip is RPC data, not light-client verified.");
            }
            break;
        case 1:
            if (latest_point(METRIC_BLOCK_INTERVAL, &latest)) {
                set_text_fmt(p->primary, "%.1f s", latest.value);
                format_integer(number, sizeof(number), latest.block_height);
                if (window_mean(&s_store->series[METRIC_BLOCK_INTERVAL], &mean, &samples)) {
                    set_text_fmt(p->secondary, "Block #%s | %s avg %.1f s over %u blocks",
                                 number, WINDOW_NAMES[s_window], mean, (unsigned)samples);
                } else {
                    set_text_fmt(p->secondary, "Block #%s interval", number);
                }
            } else {
                lv_label_set_text(p->primary, "--");
                lv_label_set_text(p->secondary, "Waiting for two consecutive headers");
            }
            lv_label_set_text(p->detail, "Header timestamps can be irregular; missing blocks stay as gaps.");
            break;
        case 2:
            if (latest_point(METRIC_TX_PER_BLOCK, &latest)) {
                set_text_fmt(p->primary, "%.0f tx", latest.value);
                format_integer(number, sizeof(number), latest.block_height);
                if (window_mean(&s_store->series[METRIC_TX_PER_BLOCK], &mean, &samples)) {
                    set_text_fmt(p->secondary, "Block #%s | %s avg %.1f tx/block",
                                 number, WINDOW_NAMES[s_window], mean);
                } else {
                    set_text_fmt(p->secondary, "Block #%s", number);
                }
            } else {
                lv_label_set_text(p->primary, "--");
                lv_label_set_text(p->secondary, "Waiting for block data");
            }
            lv_label_set_text(p->detail, "Transactions per block, including the cellbase.");
            break;
        case 3:
            if (s_snapshot.has_pool) {
                format_integer(number, sizeof(number), s_snapshot.pool_pending);
                set_text_fmt(p->primary, "%s pending", number);
                if (s_snapshot.has_fee) {
                    format_compact(extra, sizeof(extra), s_snapshot.fee_rate);
                } else {
                    snprintf(extra, sizeof(extra), "--");
                }
                set_text_fmt(p->secondary, "%llu peers | median fee %s shannon/kB | %llu kB",
                             (unsigned long long)s_snapshot.peers, extra,
                             (unsigned long long)(s_snapshot.pool_bytes / 1000));
            } else {
                lv_label_set_text(p->primary, "--");
                lv_label_set_text(p->secondary, "This endpoint does not expose pool statistics");
            }
            lv_label_set_text(p->detail, "Chart: pending transactions. Endpoint-local pool, not network-wide.");
            break;
        case 4:
            if (s_snapshot.has_difficulty) {
                format_compact(number, sizeof(number), s_snapshot.difficulty);
                set_text_fmt(p->primary, "%s", number);
                set_text_fmt(p->secondary, "Difficulty %.4e | epoch %llu",
                             s_snapshot.difficulty, (unsigned long long)s_snapshot.epoch_number);
            } else {
                lv_label_set_text(p->primary, "--");
                lv_label_set_text(p->secondary, "No valid compact target received");
            }
            lv_label_set_text(p->detail, "Difficulty from compact target; not hashrate.");
            break;
        case 5:
            update_capacity_page(p);
            break;
        case 6:
            update_light_client_page(p);
            break;
    }
}

static void refresh_ui(void)
{
    if (!s_ui_started || !s_store) {
        return;
    }
    /* Data source, unless the light client vouches for (or contradicts) it. */
    const char *source = s_snapshot.demo ? "DEMO" : (s_snapshot.stale ? "STALE" : "RPC");
    uint32_t source_color = s_snapshot.stale ? COLOR_RED : COLOR_AMBER;
    if (!s_snapshot.demo && !s_snapshot.stale && s_snapshot.lc_badge == LC_BADGE_PROVEN) {
        source = "PROVEN";
        source_color = COLOR_GREEN;
    } else if (!s_snapshot.demo && s_snapshot.lc_badge == LC_BADGE_RPC_MISMATCH) {
        source = "MISMATCH";
        source_color = COLOR_RED;
    }
    lv_label_set_text(s_source, source);
    lv_obj_set_style_text_color(s_source, lv_color_hex(source_color), 0);
    lv_label_set_text(s_status, s_snapshot.status[0] ? s_snapshot.status : "Starting data source");
    uint64_t age = s_snapshot.capture_ms > 0 && tick_ms() >= s_snapshot.capture_ms
                       ? tick_ms() - s_snapshot.capture_ms : 0;
    set_text_fmt(s_age, "%s %lus", s_snapshot.online || s_snapshot.demo ? "WiFi" : "Offline",
                          (unsigned long)(age / 1000));
    for (uint8_t i = 0; i < 7; ++i) {
        update_page_text(i);
        update_chart(&s_pages[i], i);
    }
    maybe_show_scan_results();
}

static void send_control(control_type_t type)
{
    control_event_t event = {.type = type};
    BaseType_t result = xQueueSend(s_controls, &event, 0);
    ESP_LOGI(TAG, "Control %d queued=%d", (int)type, (int)(result == pdTRUE));
}

/* Hide first, then delete on the next timer pass. Hidden objects take no
 * input, so a bouncy double tap cannot queue a second delete of the same
 * object (a double free that later crashes lv_refr while walking the tree). */
static void close_overlay(lv_obj_t *overlay)
{
    if (!overlay || lv_obj_has_flag(overlay, LV_OBJ_FLAG_USER_1)) {
        return;
    }
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_USER_1 | LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(overlay);
}

static void close_settings(lv_event_t *event)
{
    close_overlay(lv_event_get_user_data(event));
}

static void clear_history(lv_event_t *event)
{
    (void)event;
    metric_store_clear(s_store);
    s_demo_seeded = false;
    refresh_ui();
}

static void toggle_demo(lv_event_t *event)
{
    bool demo = !app_config()->demo;
    app_config_set_demo(demo);
    send_control(CONTROL_DEMO_CHANGED);
    lv_obj_t *button = lv_event_get_target(event);
    lv_obj_t *text = lv_obj_get_child(button, 0);
    lv_label_set_text(text, demo ? "Switch to RPC mode" : "Switch to DEMO mode");
}

static void reconnect(lv_event_t *event)
{
    (void)event;
    send_control(CONTROL_RECONNECT);
}

static void brightness_event(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    board_set_backlight(value);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        app_config_set_brightness(value);
    }
}

typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *title;
    lv_obj_t *ssid;
    lv_obj_t *password;
    lv_obj_t *url;
    lv_obj_t *keyboard;
    lv_obj_t *scan_label;
} network_form_t;

static network_form_t s_network_form;
static bool s_scan_requested;
/* Results are shown only for a scan that finished after this sequence. */
static uint32_t s_scan_baseline;

static void start_wifi_scan(void)
{
    s_scan_requested = true;
    s_scan_baseline = s_latest_scan_seq;
    send_control(CONTROL_SCAN_WIFI);
    if (s_network_form.scan_label) {
        lv_label_set_text(s_network_form.scan_label, "Scanning...");
    }
}

static void request_wifi_scan(lv_event_t *event)
{
    (void)event;
    start_wifi_scan();
}

static void show_network_form(void)
{
    if (s_network_form.overlay) {
        lv_obj_remove_flag(s_network_form.overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void close_scan_results(lv_event_t *event)
{
    close_overlay(lv_event_get_user_data(event));
    show_network_form();
}

static void select_scan_result(lv_event_t *event)
{
    uint8_t index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    if (s_network_form.overlay && index < s_scanned_count) {
        /* A different network almost always needs a different password. */
        if (strcmp(lv_textarea_get_text(s_network_form.ssid), s_scanned_ssids[index]) != 0) {
            lv_textarea_set_text(s_network_form.password, "");
        }
        lv_textarea_set_text(s_network_form.ssid, s_scanned_ssids[index]);
        lv_keyboard_set_textarea(s_network_form.keyboard, s_network_form.password);
    }
    lv_obj_t *list_screen = lv_obj_get_parent(lv_event_get_target(event));
    while (list_screen && lv_obj_get_parent(list_screen) != lv_screen_active()) {
        list_screen = lv_obj_get_parent(list_screen);
    }
    close_overlay(list_screen);
    show_network_form();
}

static void maybe_show_scan_results(void)
{
    if (!s_scan_requested || s_latest_scan_seq == s_scan_baseline || !s_network_form.overlay) {
        return;
    }
    s_scan_requested = false;
    if (s_network_form.scan_label) {
        lv_label_set_text(s_network_form.scan_label, "Scan");
    }
    lv_obj_add_flag(s_network_form.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *scan = lv_obj_create(lv_screen_active());
    lv_obj_set_size(scan, 480, 480);
    lv_obj_set_pos(scan, 0, 0);
    lv_obj_set_style_bg_color(scan, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scan, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scan, 0, 0);
    label(scan, "Available networks", 16, 12, 360, &lv_font_montserrat_24, lv_color_hex(COLOR_TEXT));
    lv_obj_t *back = lv_button_create(scan);
    lv_obj_set_size(back, 48, 48);
    lv_obj_set_pos(back, 408, 4);
    style_button(back, false);
    button_label(back, "X", &lv_font_montserrat_20, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(back, close_scan_results, LV_EVENT_CLICKED, scan);
    lv_obj_t *list = lv_list_create(scan);
    lv_obj_set_size(list, 448, 400);
    lv_obj_set_pos(list, 16, 64);
    lv_obj_set_style_bg_color(list, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(COLOR_TEXT), 0);
    if (s_scanned_count == 0) {
        lv_list_add_text(list, s_latest_scan_ok ? "No networks found. Go back and retry Scan."
                                                : "Wi-Fi scan failed. Go back and retry Scan.");
        return;
    }
    for (uint8_t i = 0; i < s_scanned_count; ++i) {
        char entry[64];
        snprintf(entry, sizeof(entry), "%s  (%d dBm)", s_scanned_ssids[i], s_scanned_rssi[i]);
        lv_obj_t *button = lv_list_add_button(list, LV_SYMBOL_WIFI, entry);
        lv_obj_set_style_min_height(button, 48, 0);
        lv_obj_add_event_cb(button, select_scan_result, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

static void focus_network_input(lv_event_t *event)
{
    lv_keyboard_set_textarea(s_network_form.keyboard, lv_event_get_target(event));
}

static lv_obj_t *network_input(lv_obj_t *parent, const char *value, int y,
                               size_t max_length, bool password)
{
    lv_obj_t *input = lv_textarea_create(parent);
    lv_obj_set_size(input, 432, 40);
    lv_obj_set_pos(input, 24, y);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, max_length);
    lv_textarea_set_text(input, value);
    lv_textarea_set_password_mode(input, password);
    lv_obj_set_style_bg_color(input, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_text_color(input, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_border_color(input, lv_color_hex(COLOR_BLUE), 0);
    lv_obj_set_style_radius(input, 8, 0);
    lv_obj_add_event_cb(input, focus_network_input, LV_EVENT_CLICKED, NULL);
    return input;
}

static void close_network_form(lv_event_t *event)
{
    memset(&s_network_form, 0, sizeof(s_network_form));
    s_scan_requested = false;
    close_settings(event);
}

static void save_network_settings(lv_event_t *event)
{
    bool public_rpc = (bool)(uintptr_t)lv_event_get_user_data(event);
    const char *ssid = lv_textarea_get_text(s_network_form.ssid);
    const char *password = lv_textarea_get_text(s_network_form.password);
    const char *url = lv_textarea_get_text(s_network_form.url);
    if (ssid[0] == '\0' || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)) {
        lv_label_set_text(s_network_form.title, "SSID and valid HTTP(S) URL required");
        lv_obj_set_style_text_color(s_network_form.title, lv_color_hex(COLOR_RED), 0);
        return;
    }
    if (app_config_set_network(ssid, password, url, public_rpc) != ESP_OK) {
        lv_label_set_text(s_network_form.title, "Could not save network settings");
        lv_obj_set_style_text_color(s_network_form.title, lv_color_hex(COLOR_RED), 0);
        return;
    }
    metric_store_clear(s_store);
    s_demo_seeded = false;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    send_control(CONTROL_CONFIG_CHANGED);
    lv_obj_t *overlay = s_network_form.overlay;
    memset(&s_network_form, 0, sizeof(s_network_form));
    s_scan_requested = false;
    close_overlay(overlay);
    refresh_ui();
}

static void open_network_settings(lv_event_t *event)
{
    ESP_LOGI(TAG, "Opening Wi-Fi provisioning");
    lv_obj_t *settings = lv_event_get_user_data(event);
    if (s_network_form.overlay || (settings && lv_obj_has_flag(settings, LV_OBJ_FLAG_USER_1))) {
        return; /* Already open: ignore a repeated tap. */
    }
    close_overlay(settings);
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    memset(&s_network_form, 0, sizeof(s_network_form));
    s_network_form.overlay = overlay;
    lv_obj_set_size(overlay, 480, 480);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    s_network_form.title = label(overlay, "Wi-Fi / node", 24, 12, 360,
                                 &lv_font_montserrat_24, lv_color_hex(COLOR_TEXT));
    lv_obj_t *close = lv_button_create(overlay);
    lv_obj_set_size(close, 48, 48);
    lv_obj_set_pos(close, 408, 4);
    style_button(close, false);
    button_label(close, "X", &lv_font_montserrat_20, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(close, close_network_form, LV_EVENT_CLICKED, overlay);
    label(overlay, "Wi-Fi SSID", 24, 48, 200, &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
    s_network_form.ssid = network_input(overlay, app_config()->ssid, 64, APP_SSID_LEN, false);
    lv_obj_set_width(s_network_form.ssid, 312);
    lv_obj_t *scan = lv_button_create(overlay);
    lv_obj_set_size(scan, 120, 44);
    lv_obj_set_pos(scan, 344, 64);
    style_button(scan, true);
    s_network_form.scan_label = button_label(scan, "Scan", &lv_font_montserrat_14, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(scan, request_wifi_scan, LV_EVENT_CLICKED, NULL);
    label(overlay, "Password", 24, 108, 200, &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
    s_network_form.password = network_input(overlay, app_config()->password, 124, APP_PASSWORD_LEN, true);
    label(overlay, "CKB JSON-RPC URL (mainnet)", 24, 168, 360, &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
    s_network_form.url = network_input(overlay, app_config()->rpc_url, 184, APP_URL_LEN, false);
    lv_obj_t *save_public = lv_button_create(overlay);
    lv_obj_set_size(save_public, 208, 44);
    lv_obj_set_pos(save_public, 24, 232);
    style_button(save_public, true);
    button_label(save_public, "Save public RPC", &lv_font_montserrat_14, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(save_public, save_network_settings, LV_EVENT_CLICKED, (void *)(uintptr_t)true);
    lv_obj_t *save_lan = lv_button_create(overlay);
    lv_obj_set_size(save_lan, 208, 44);
    lv_obj_set_pos(save_lan, 248, 232);
    style_button(save_lan, false);
    button_label(save_lan, "Save LAN node", &lv_font_montserrat_14, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(save_lan, save_network_settings, LV_EVENT_CLICKED, (void *)(uintptr_t)false);
    s_network_form.keyboard = lv_keyboard_create(overlay);
    lv_obj_set_size(s_network_form.keyboard, 480, 200);
    lv_obj_set_pos(s_network_form.keyboard, 0, 280);
    lv_keyboard_set_textarea(s_network_form.keyboard, s_network_form.ssid);
    start_wifi_scan();
}

static void open_settings(lv_event_t *event)
{
    (void)event;
    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay, 480, 480);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    label(overlay, "Settings", 24, 16, 300, &lv_font_montserrat_32, lv_color_hex(COLOR_TEXT));
    lv_obj_t *close = lv_button_create(overlay);
    lv_obj_set_size(close, 48, 48);
    lv_obj_set_pos(close, 408, 12);
    style_button(close, false);
    button_label(close, "X", &lv_font_montserrat_20, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(close, close_settings, LV_EVENT_CLICKED, overlay);
    lv_obj_t *network = lv_button_create(overlay);
    lv_obj_set_size(network, 432, 48);
    lv_obj_set_pos(network, 24, 80);
    style_button(network, true);
    button_label(network, "Wi-Fi / node provisioning", &lv_font_montserrat_16, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(network, open_network_settings, LV_EVENT_CLICKED, overlay);
    lv_obj_t *mode = lv_button_create(overlay);
    lv_obj_set_size(mode, 432, 48);
    lv_obj_set_pos(mode, 24, 144);
    style_button(mode, true);
    button_label(mode, app_config()->demo ? "Switch to RPC mode" : "Switch to DEMO mode",
                 &lv_font_montserrat_16, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(mode, toggle_demo, LV_EVENT_CLICKED, NULL);
    label(overlay, "Brightness", 24, 216, 200, &lv_font_montserrat_16, lv_color_hex(COLOR_TEXT));
    lv_obj_t *slider = lv_slider_create(overlay);
    lv_obj_set_size(slider, 432, 24);
    lv_obj_set_pos(slider, 24, 248);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, app_config()->brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, brightness_event, LV_EVENT_RELEASED, NULL);
    lv_obj_t *reconnect_button = lv_button_create(overlay);
    lv_obj_set_size(reconnect_button, 208, 48);
    lv_obj_set_pos(reconnect_button, 24, 304);
    style_button(reconnect_button, false);
    button_label(reconnect_button, "Reconnect", &lv_font_montserrat_16, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(reconnect_button, reconnect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear = lv_button_create(overlay);
    lv_obj_set_size(clear, 208, 48);
    lv_obj_set_pos(clear, 248, 304);
    style_button(clear, false);
    button_label(clear, "Clear history", &lv_font_montserrat_16, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(clear, clear_history, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done = lv_button_create(overlay);
    lv_obj_set_size(done, 432, 48);
    lv_obj_set_pos(done, 384 - 16, 384);
    style_button(done, true);
    button_label(done, "Done", &lv_font_montserrat_16, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(done, close_settings, LV_EVENT_CLICKED, overlay);
}

/* Page layout inside the 336 px body (screen y 48..384):
 *   0..46   primary value (40 px font) + range button
 *  52..72   secondary line (live context)
 *  80..248  chart, with y-axis labels in the left 56 px
 * 256..274  coverage line
 * 278..314  detail (max two lines)
 * The status bar sits at 388 and the nav bar at 408, so nothing overlaps. */

static void create_page(lv_obj_t *body, uint8_t index)
{
    page_t *p = &s_pages[index];
    p->root = lv_obj_create(body);
    lv_obj_set_size(p->root, 480, PAGE_HEIGHT);
    lv_obj_set_pos(p->root, 0, 0);
    lv_obj_set_style_bg_color(p->root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_radius(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *range = lv_button_create(p->root);
    lv_obj_set_size(range, 80, 40);
    lv_obj_set_pos(range, 384, 4);
    style_button(range, false);
    p->range = button_label(range, WINDOW_NAMES[s_window], &lv_font_montserrat_16, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(range, range_event, LV_EVENT_CLICKED, NULL);
    p->primary = label(p->root, "--", 16, 0, 360, &lv_font_montserrat_40, lv_color_hex(COLOR_TEXT));
    single_line(p->primary, &lv_font_montserrat_40);
    p->secondary = label(p->root, "Waiting for data", 16, 52, 448, &lv_font_montserrat_16, lv_color_hex(COLOR_BLUE));
    single_line(p->secondary, &lv_font_montserrat_16);
    p->chart = lv_chart_create(p->root);
    lv_obj_set_size(p->chart, CHART_W, CHART_H);
    lv_obj_set_pos(p->chart, CHART_X, CHART_Y);
    lv_chart_set_type(p->chart, index == 2 ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(p->chart, 2);
    lv_chart_set_range(p->chart, LV_CHART_AXIS_PRIMARY_Y, 0, CHART_RANGE);
    /* Three horizontal grid lines line up with the max/mid/min labels. */
    lv_chart_set_div_line_count(p->chart, 3, 0);
    lv_obj_set_style_pad_all(p->chart, 0, 0);
    lv_obj_set_style_bg_color(p->chart, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_color(p->chart, lv_color_hex(COLOR_GRID), 0);
    lv_obj_set_style_line_color(p->chart, lv_color_hex(COLOR_GRID), LV_PART_MAIN);
    lv_obj_set_style_line_width(p->chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(p->chart, 0, 0, LV_PART_INDICATOR); /* no per-point dots */
    lv_obj_set_style_pad_column(p->chart, 2, 0);             /* bar spacing */
    p->series = lv_chart_add_series(p->chart, lv_color_hex(index == 4 ? COLOR_BLUE : COLOR_GREEN),
                                    LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(p->chart, chart_event, LV_EVENT_PRESSING, p);
    for (int i = 0; i < 3; ++i) {
        int y = CHART_Y + i * CHART_H / 2 - 8;
        p->axis[i] = label(p->root, "", 0, y, CHART_X - 6, &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
        lv_obj_set_style_text_align(p->axis[i], LV_TEXT_ALIGN_RIGHT, 0);
        single_line(p->axis[i], &lv_font_montserrat_14);
    }
    p->coverage = label(p->root, "No history yet", 16, 256, 448, &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
    single_line(p->coverage, &lv_font_montserrat_14);
    p->detail = label(p->root, "", 16, 278, 448, &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
}

static void create_shell(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_add_event_cb(screen, gesture_event, LV_EVENT_GESTURE, NULL);
    s_source = label(screen, "RPC", 12, 16, 76, &lv_font_montserrat_14, lv_color_hex(COLOR_AMBER));
    single_line(s_source, &lv_font_montserrat_14);
    s_title = label(screen, PAGE_NAMES[0], 88, 8, 216, &lv_font_montserrat_24, lv_color_hex(COLOR_TEXT));
    s_age = label(screen, "Offline", 304, 16, 112, &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
    lv_obj_t *settings = lv_button_create(screen);
    lv_obj_set_size(settings, 48, 48);
    lv_obj_set_pos(settings, 424, 0);
    style_button(settings, false);
    button_label(settings, LV_SYMBOL_SETTINGS, &lv_font_montserrat_20, lv_color_hex(COLOR_TEXT));
    lv_obj_add_event_cb(settings, open_settings, LV_EVENT_CLICKED, NULL);

    lv_obj_t *body = lv_obj_create(screen);
    lv_obj_set_size(body, 480, PAGE_HEIGHT);
    lv_obj_set_pos(body, 0, 48);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    for (uint8_t i = 0; i < 7; ++i) {
        create_page(body, i);
    }

    lv_obj_t *nav = lv_obj_create(screen);
    lv_obj_set_size(nav, 480, 72);
    lv_obj_set_pos(nav, 0, 408);
    lv_obj_set_style_bg_color(nav, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_all(nav, 0, 0);
    for (uint8_t i = 0; i < 7; ++i) {
        s_nav[i] = lv_button_create(nav);
        lv_obj_set_size(s_nav[i], 64, 48);
        lv_obj_set_pos(s_nav[i], 4 + i * 68, 12);
        style_button(s_nav[i], false);
        lv_obj_t *nav_text = button_label(s_nav[i], NAV_NAMES[i], &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
        /* Inherit the button's colour so the active tab shows green. */
        lv_obj_remove_local_style_prop(nav_text, LV_STYLE_TEXT_COLOR, 0);
        lv_obj_add_event_cb(s_nav[i], nav_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    s_status = label(screen, "Starting", 16, 388, 448, &lv_font_montserrat_14, lv_color_hex(COLOR_MUTED));
    single_line(s_status, &lv_font_montserrat_14);
    show_page(0);
}

#if CONFIG_CKB_DEBUG_SCREENSHOTS
static void shot_init(void);
static void shot_step(void);
#endif

static void ui_task(void *arg)
{
    (void)arg;
    s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_lock) {
        ESP_LOGE(TAG, "Cannot create LVGL lock");
        vTaskDelete(NULL);
    }
    s_store = heap_caps_malloc(sizeof(*s_store), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_store) {
        ESP_LOGE(TAG, "Cannot allocate metric history in PSRAM");
        vTaskDelete(NULL);
    }
    metric_store_init(s_store);
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = lv_display_create(BOARD_HRES, BOARD_VRES);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush_display);
    s_vsync = xSemaphoreCreateBinary();
    const esp_lcd_rgb_panel_event_callbacks_t panel_callbacks = {.on_vsync = on_vsync};
    if (!s_vsync ||
        esp_lcd_rgb_panel_register_event_callbacks(board_panel(), &panel_callbacks, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot register RGB vsync callback");
        vTaskDelete(NULL);
    }
    void *frame0 = NULL;
    void *frame1 = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(board_panel(), 2, &frame0, &frame1) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot get RGB framebuffers");
        vTaskDelete(NULL);
    }
    lv_display_set_buffers(display, frame0, frame1, BOARD_HRES * BOARD_VRES * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_touch);
    lv_indev_set_display(input, display);
    xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
    create_shell();
    s_ui_started = true;
    xSemaphoreGiveRecursive(s_lvgl_lock);
    uint32_t refresh = 0;
    uint32_t last_memory_log = tick_ms();
    /* Watch this task: a deadlock on the LVGL lock or a stuck render used to
     * freeze the panel forever, because the watchdog only watched idle tasks
     * and did not panic. Now a stall of CONFIG_ESP_TASK_WDT_TIMEOUT_S reboots
     * the board and leaves a core dump naming the stuck call. */
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
#if CONFIG_CKB_DEBUG_SCREENSHOTS
    shot_init();
#endif
    while (true) {
        esp_task_wdt_reset();
        static dashboard_snapshot_t snapshot;
        bool changed = false;
        while (xQueueReceive(s_snapshots, &snapshot, 0) == pdTRUE) {
            ingest_snapshot(&snapshot);
            changed = true;
        }
        xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
        if (changed || tick_ms() - refresh >= 500) {
            refresh_ui();
            refresh = tick_ms();
        }
#if CONFIG_CKB_DEBUG_SCREENSHOTS
        shot_step();
#endif
        lv_timer_handler();
        xSemaphoreGiveRecursive(s_lvgl_lock);
        if (tick_ms() - last_memory_log >= 30000) {
            last_memory_log = tick_ms();
            lv_mem_monitor_t mem;
            lv_mem_monitor(&mem);
            ESP_LOGI(TAG, "LVGL pool used=%u%% free=%u biggest=%u frag=%u%%",
                     (unsigned)mem.used_pct, (unsigned)mem.free_size,
                     (unsigned)mem.free_biggest_size, (unsigned)mem.frag_pct);
            ESP_LOGI(TAG, "UI stack hwm=%u internal=%u internal_min=%u psram=%u psram_min=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

#if CONFIG_CKB_DEBUG_SCREENSHOTS
/* Debug screen captures (Kconfig CKB_DEBUG_SCREENSHOTS). The UI task shows
 * each page and renders it into a private PSRAM buffer with lv_snapshot
 * (fast, under the LVGL lock); shot_task then prints it as SHOT_* lines.
 * Console output busy-waits on the UART, so shot_task runs at idle priority:
 * above idle it would starve IDLE and trip the task watchdog. */
static uint16_t *s_shot_px;
static lv_draw_buf_t s_shot_buf;
static QueueHandle_t s_shot_jobs;
static SemaphoreHandle_t s_shot_done;
static int8_t s_shot_page = -1;      /* page being captured; -1 = idle */
static bool s_shot_streaming;
static uint8_t s_shot_return_page;
static uint64_t s_shot_due_ms;

static void shot_emit(const char *line, void *ctx)
{
    (void)ctx;
    printf("%s\n", line);
}

static void shot_task(void *arg)
{
    (void)arg;
    uint8_t page;
    for (;;) {
        if (xQueueReceive(s_shot_jobs, &page, portMAX_DELAY) == pdTRUE) {
            shot_stream(page, s_shot_px, s_shot_buf.header.w, s_shot_buf.header.h,
                        s_shot_buf.header.stride / 2, shot_emit, NULL);
            fflush(stdout);
            xSemaphoreGive(s_shot_done);
        }
    }
}

static void shot_init(void)
{
    size_t size = (size_t)BOARD_HRES * BOARD_VRES * 2;
    s_shot_px = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_shot_jobs = xQueueCreate(1, sizeof(uint8_t));
    s_shot_done = xSemaphoreCreateBinary();
    if (!s_shot_px || !s_shot_jobs || !s_shot_done ||
        lv_draw_buf_init(&s_shot_buf, BOARD_HRES, BOARD_VRES, LV_COLOR_FORMAT_RGB565, 0,
                         s_shot_px, size) != LV_RESULT_OK ||
        xTaskCreatePinnedToCore(shot_task, "shot", 4096, NULL, tskIDLE_PRIORITY, NULL, 1) != pdTRUE) {
        ESP_LOGE(TAG, "screen capture disabled: setup failed");
        s_shot_due_ms = UINT64_MAX;
        return;
    }
    s_shot_due_ms = tick_ms() + (uint64_t)CONFIG_CKB_DEBUG_SCREENSHOTS_FIRST_S * 1000ULL;
    ESP_LOGI(TAG, "screen capture armed: first round in %d s", CONFIG_CKB_DEBUG_SCREENSHOTS_FIRST_S);
}

static void shot_round_end(const char *why)
{
    show_page(s_shot_return_page);
    s_shot_page = -1;
    s_shot_due_ms = tick_ms() + (uint64_t)CONFIG_CKB_DEBUG_SCREENSHOTS_PERIOD_S * 1000ULL;
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
    ESP_LOGI(TAG, "screen capture round %s", why);
}

/* Called by the UI task with the LVGL lock held. */
static void shot_step(void)
{
    if (s_shot_page < 0) {
        if (tick_ms() < s_shot_due_ms) {
            return;
        }
        if (!s_snapshot.demo && !s_snapshot.rpc_ok) {
            s_shot_due_ms = tick_ms() + 10000; /* wait for live data */
            return;
        }
        s_shot_return_page = s_page;
        s_shot_page = 0;
        ESP_LOGI(TAG, "screen capture round starting (logs quiet until done)");
        esp_log_level_set("*", ESP_LOG_WARN); /* keep the console for SHOT_* */
    } else if (s_shot_streaming) {
        if (xSemaphoreTake(s_shot_done, 0) != pdTRUE) {
            return;
        }
        s_shot_streaming = false;
        if (++s_shot_page >= 7) {
            shot_round_end("done");
            return;
        }
    }
    show_page((uint8_t)s_shot_page);
    refresh_ui();
    lv_obj_update_layout(lv_screen_active());
    if (lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &s_shot_buf) != LV_RESULT_OK) {
        shot_round_end("aborted: snapshot failed");
        return;
    }
    uint8_t page = (uint8_t)s_shot_page;
    xQueueSend(s_shot_jobs, &page, 0);
    s_shot_streaming = true;
}
#endif

esp_err_t dashboard_ui_start(QueueHandle_t snapshots, QueueHandle_t controls)
{
    if (!snapshots || !controls) {
        return ESP_ERR_INVALID_ARG;
    }
    s_snapshots = snapshots;
    s_controls = controls;
    return xTaskCreatePinnedToCore(ui_task, "lvgl_ui", 16384, NULL, 6, NULL, 1) == pdTRUE
               ? ESP_OK : ESP_ERR_NO_MEM;
}
