// Strava Exercise Load Monitor
// LilyGo T-Display S3 + ST7789V 170x320 IPS display, LVGL v9

#include <WiFi.h>
#include <Arduino.h>
#include <lvgl.h>
#include "PINS_T_DISPLAY_S3.h"
#include "secrets.h"
#include "config.h"
#include "strava_api.h"
#include "display_calorie_trend.h"
#include "display_grid.h"
#include "display_stats.h"
#include "display_status.h"
#include "web_server.h"
#include "ui_fonts.h"
#include "screen_brightness.h"
#include "mqtt_client.h"

static Config     g_cfg;
static StravaData g_data;
static lv_obj_t  *g_screen_status = nullptr;
static lv_obj_t  *g_screen_grid  = nullptr;
static lv_obj_t  *g_screen_stats = nullptr;
static lv_obj_t  *g_screen_calories = nullptr;
static lv_obj_t  *g_screen_calorie_trend = nullptr;
static uint8_t    g_screen_index = 0;
static bool       g_display_ready = false;
static uint32_t   g_last_fetch_ms = 0;
static uint32_t   g_last_fetch_attempt_ms = 0;
static bool       g_trend_refresh_pending = true;
static lv_display_t *disp = nullptr;
static lv_color_t *g_lvgl_buf = nullptr;
static lv_obj_t  *g_network_overlay = nullptr;
static lv_timer_t *g_network_overlay_timer = nullptr;
static uint32_t   g_last_status_refresh_ms = 0;

struct ButtonState {
    uint8_t pin;
    bool last_raw_pressed;
    bool stable_pressed;
    bool long_press_handled;
    uint32_t last_change_ms;
    uint32_t pressed_ms;
};

static ButtonState g_next_button = { BTN_A, false, false, false, 0, 0 };
static ButtonState g_info_button = { BTN_B, false, false, false, 0, 0 };

enum ScreenIndex {
    SCREEN_STATUS = 0,
    SCREEN_LOAD_GRID,
    SCREEN_STATS,
    SCREEN_CALORIES_GRID,
    SCREEN_CALORIE_TREND,
    SCREEN_COUNT,
};

static void load_screen_index(uint8_t index, bool animate);
static void refresh_screen_selection(bool prefer_primary);
static void update_status_screen();
static void do_fetch();
static void refresh_trend_screen_if_needed();

static const char *screen_name(uint8_t index) {
    switch (index) {
        case SCREEN_STATUS:        return "status";
        case SCREEN_LOAD_GRID:     return "load-grid";
        case SCREEN_STATS:         return "stats";
        case SCREEN_CALORIES_GRID: return "calories-grid";
        case SCREEN_CALORIE_TREND: return "calorie-trend";
        default:                   return "unknown";
    }
}

static lv_obj_t *screen_for_index(uint8_t index) {
    switch (index) {
        case SCREEN_STATUS:        return g_screen_status;
        case SCREEN_LOAD_GRID:     return g_screen_grid;
        case SCREEN_STATS:         return g_screen_stats;
        case SCREEN_CALORIES_GRID: return g_screen_calories;
        case SCREEN_CALORIE_TREND: return g_screen_calorie_trend;
        default:                   return nullptr;
    }
}

static uint32_t screen_period_ms(uint8_t index) {
    if (index == SCREEN_STATS) return (uint32_t)g_cfg.screen_switch_secs * 1000UL;
    return (uint32_t)g_cfg.screen_switch_secs * 3000UL;
}

static bool has_valid_dataset() {
    return g_data.valid;
}

static bool screen_is_available(uint8_t index) {
    if (!screen_for_index(index)) return false;

    switch (index) {
        case SCREEN_STATUS:
            return !has_valid_dataset();
        case SCREEN_LOAD_GRID:
        case SCREEN_STATS:
        case SCREEN_CALORIES_GRID:
        case SCREEN_CALORIE_TREND:
            return has_valid_dataset();
        default:
            return false;
    }
}

static uint8_t first_available_screen() {
    for (uint8_t index = 0; index < SCREEN_COUNT; ++index) {
        if (screen_is_available(index)) return index;
    }
    return SCREEN_STATUS;
}

static const char *wifi_status_text(wl_status_t status) {
    switch (status) {
        case WL_CONNECTED:      return "connected";
        case WL_NO_SHIELD:      return "radio unavailable";
        case WL_IDLE_STATUS:    return "idle";
        case WL_NO_SSID_AVAIL:  return "ssid unavailable";
        case WL_SCAN_COMPLETED: return "scan completed";
        case WL_CONNECT_FAILED: return "connect failed";
        case WL_CONNECTION_LOST:return "connection lost";
        case WL_DISCONNECTED:   return "disconnected";
        default:                return "unknown";
    }
}

static void style_active_screen_black() {
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

static void hide_network_overlay() {
    if (g_network_overlay_timer) {
        lv_timer_delete(g_network_overlay_timer);
        g_network_overlay_timer = nullptr;
    }
    if (g_network_overlay) {
        lv_obj_del(g_network_overlay);
        g_network_overlay = nullptr;
    }
}

static bool wifi_ap_mode_active() {
    wifi_mode_t mode = WiFi.getMode();
    return mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
}

static String current_ip_string() {
    if (wifi_ap_mode_active()) return WiFi.softAPIP().toString();
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
    return String("not connected");
}

static String current_wifi_ssid_string() {
    if (WiFi.status() == WL_CONNECTED) {
        String ssid = WiFi.SSID();
        if (ssid.length() > 0) return ssid;
    }
    return String("not connected");
}

static String current_ap_name_string() {
    String ap_name = WiFi.softAPSSID();
    if (ap_name.length() > 0) return ap_name;
    return String(kSetupApSsid);
}

static void network_overlay_timeout_cb(lv_timer_t *) {
    hide_network_overlay();
}

static void show_network_overlay() {
    hide_network_overlay();

    String text;
    text.reserve(128);
    text += "IP: ";
    text += current_ip_string();
    text += "\nWi-Fi: ";
    text += current_wifi_ssid_string();
    text += "\nAP: ";
    text += current_ap_name_string();

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    g_network_overlay = overlay;
    lv_obj_set_size(overlay, 280, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(overlay, 1, 0);
    lv_obj_set_style_border_color(overlay, lv_color_hex(0x39d353), 0);
    lv_obj_set_style_radius(overlay, 12, 0);
    lv_obj_set_style_pad_hor(overlay, 12, 0);
    lv_obj_set_style_pad_ver(overlay, 10, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(overlay, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *label = lv_label_create(overlay);
    lv_obj_set_width(label, 256);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_font_label(), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text.c_str());
    lv_obj_center(label);

    g_network_overlay_timer = lv_timer_create(network_overlay_timeout_cb, 4000, nullptr);
}

static void force_device_reboot() {
    hide_network_overlay();
    Serial.println("[Buttons] Long press detected - rebooting");
    delay(50);
    ESP.restart();
}

// ── LVGL callbacks ────────────────────────────────────────────────────────────

static uint32_t millis_cb() { return millis(); }

static void my_disp_flush(lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map,
                            lv_area_get_width(area), lv_area_get_height(area));
    lv_disp_flush_ready(d);
}

#if LV_USE_LOG != 0
static void my_log_print(lv_log_level_t level, const char *buf) {
    LV_UNUSED(level);
    Serial.println(buf);
}
#endif

// ── Display brightness ────────────────────────────────────────────────────────

static void apply_brightness(uint8_t level) {
    static bool backlight_initialized = false;
    static uint8_t current_level = 0;
    static constexpr uint8_t kBacklightSteps = 16;

    if (level > kBacklightSteps) level = kBacklightSteps;

    if (!backlight_initialized) {
        pinMode(GFX_BL, OUTPUT);
        digitalWrite(GFX_BL, LOW);
        backlight_initialized = true;
    }

    // The LilyGo T-Display S3 backlight is controlled by a 16-step driver on
    // GPIO38. It expects pulses to advance brightness rather than a PWM duty.
    if (level == 0) {
        digitalWrite(GFX_BL, LOW);
        delay(3);
        current_level = 0;
        return;
    }

    if (current_level == 0) {
        digitalWrite(GFX_BL, HIGH);
        current_level = kBacklightSteps;
        delayMicroseconds(30);
    }

    int from = kBacklightSteps - current_level;
    int to = kBacklightSteps - level;
    int pulses = (kBacklightSteps + to - from) % kBacklightSteps;
    for (int i = 0; i < pulses; ++i) {
        digitalWrite(GFX_BL, LOW);
        digitalWrite(GFX_BL, HIGH);
    }
    current_level = level;
}

// Public API: set brightness by percentage (0-100). Maps to the panel's 16
// hardware backlight levels, with 0 as a hard off state.
void set_screen_brightness_pct(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint8_t level = 0;
    if (pct != 0) {
        level = (uint8_t)((pct * 16UL + 99UL) / 100UL);
        if (level == 0) level = 1;
    }
    apply_brightness(level);
}

// ── Screen switch timer ───────────────────────────────────────────────────────
// Grid screens stay visible 3x longer than stats to give the activity view priority.

static lv_timer_t *g_screen_switch_timer = nullptr;

static void force_screen_redraw(lv_obj_t *scr) {
    if (!scr) return;
    lv_obj_invalidate(scr);
}

static void load_screen_index(uint8_t index, bool animate) {
    (void)animate;
    uint8_t requested_index = index;

    if (!screen_is_available(index)) {
        index = first_available_screen();
    }

    lv_obj_t *next = screen_for_index(index);
    if (!next) return;

    bool is_change = (index != g_screen_index) || (next != lv_scr_act());
    if (is_change) {
        Serial.printf("[Screen] %s -> %s (requested=%s)\n",
                      screen_name(g_screen_index),
                      screen_name(index),
                      screen_name(requested_index));
    }

    uint8_t old_index = g_screen_index;
    g_screen_index = index;
    bool did_load = (next != lv_scr_act());
    if (did_load) {
        // Stop animations on the screen we're leaving (prevents LVGL dirty-area
        // loops from inactive-screen animation callbacks on the next timer tick).
        if (old_index == SCREEN_LOAD_GRID)     display_grid_stop_animations(GRID_METRIC_LOAD);
        if (old_index == SCREEN_CALORIES_GRID) display_grid_stop_animations(GRID_METRIC_CALORIES);

        lv_scr_load(next);
        Serial.printf("[Screen] Active: %s\n", screen_name(g_screen_index));

        // Start animations on the screen we're entering.
        if (index == SCREEN_LOAD_GRID && has_valid_dataset())
            display_grid_start_animations(GRID_METRIC_LOAD, g_data, g_cfg);
        if (index == SCREEN_CALORIES_GRID && has_valid_dataset())
            display_grid_start_animations(GRID_METRIC_CALORIES, g_data, g_cfg);
    }

    if (index == SCREEN_CALORIE_TREND) {
        refresh_trend_screen_if_needed();
    }

    if (did_load) {
        force_screen_redraw(next);
    }

    if (g_screen_switch_timer) {
        lv_timer_set_period(g_screen_switch_timer, screen_period_ms(g_screen_index));
        lv_timer_reset(g_screen_switch_timer);
    }
}

static void advance_to_next_screen() {
    for (int step = 0; step < SCREEN_COUNT; step++) {
        uint8_t next_index = (g_screen_index + 1 + step) % SCREEN_COUNT;
        if (!screen_is_available(next_index)) continue;
        Serial.printf("[Screen] Advancing to %s\n", screen_name(next_index));
        load_screen_index(next_index, true);
        return;
    }
}

static void screen_switch_cb(lv_timer_t *t) {
    for (int step = 0; step < SCREEN_COUNT; step++) {
        uint8_t next_index = (g_screen_index + 1 + step) % SCREEN_COUNT;
        if (!screen_is_available(next_index)) continue;
        Serial.printf("[Screen] Timer flip to %s\n", screen_name(next_index));
        load_screen_index(next_index, true);
        return;
    }

    lv_obj_t *next = screen_for_index(first_available_screen());
    if (!next) return;
    lv_timer_set_period(t, screen_period_ms(g_screen_index));
}

static void handle_short_next_press() {
    Serial.println("[Buttons] NEXT short press");
    advance_to_next_screen();
}

static void handle_short_info_press() {
    Serial.println("[Buttons] INFO short press");
    show_network_overlay();
}

static void button_tick(ButtonState &button, void (*short_press_cb)()) {
    uint32_t now = millis();
    bool raw_pressed = (digitalRead(button.pin) == LOW);

    if (raw_pressed != button.last_raw_pressed) {
        button.last_raw_pressed = raw_pressed;
        button.last_change_ms = now;
    }

    if (now - button.last_change_ms < 30) return;

    if (raw_pressed != button.stable_pressed) {
        button.stable_pressed = raw_pressed;
        if (button.stable_pressed) {
            Serial.printf("[Buttons] Pin %u pressed\n", (unsigned)button.pin);
            button.pressed_ms = now;
            button.long_press_handled = false;
        } else if (!button.long_press_handled && short_press_cb) {
            Serial.printf("[Buttons] Pin %u released\n", (unsigned)button.pin);
            short_press_cb();
        }
        return;
    }

    if (button.stable_pressed && !button.long_press_handled && now - button.pressed_ms >= 1200) {
        button.long_press_handled = true;
        Serial.printf("[Buttons] Pin %u long press\n", (unsigned)button.pin);
        force_device_reboot();
    }
}

static void buttons_tick() {
    button_tick(g_next_button, handle_short_next_press);
    button_tick(g_info_button, handle_short_info_press);
}

// ── Async data refresh via FreeRTOS task ──────────────────────────────────────

// The HTTP client has its own 15 s connect + 15 s read timeouts, so a single
// attempt takes at most ~30 s. With 3 retries + 5 s gaps the worst case is
// ~115 s. Set the outer watchdog well above that so it only fires on a true hang.
static const uint32_t FETCH_TIMEOUT_MS  = 120000;
static const int      FETCH_MAX_RETRIES = 3;
static const uint32_t FETCH_RETRY_MS    = 5000;

// State shared between main loop and fetch task.
// g_fetch_abort is written by the main loop and read by the task; volatile is
// sufficient because it is only ever set true (no torn read/write issue).
// g_fetch_task is written by BOTH the task (self-null on exit) and the main loop
// watchdog — access must be guarded by g_fetch_task_mux on SMP cores.
enum FetchState { FETCH_IDLE, FETCH_RUNNING, FETCH_DONE_OK, FETCH_DONE_FAIL };
static volatile FetchState g_fetch_state = FETCH_IDLE;
static volatile bool       g_fetch_abort = false; // ask task to stop early
static StravaData          g_fetch_result;
static TaskHandle_t        g_fetch_task  = nullptr;
static portMUX_TYPE        g_fetch_task_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t   g_fetch_started_ms = 0;
static volatile uint32_t   g_fetch_abort_ms   = 0; // non-zero while waiting for task to exit

static void fetch_task(void *) {
    bool ok = false;
    for (int attempt = 1; attempt <= FETCH_MAX_RETRIES && !g_fetch_abort; attempt++) {
        Serial.printf("[Fetch] Attempt %d/%d (abort=%d)\n", attempt, FETCH_MAX_RETRIES, (int)g_fetch_abort);
        uint32_t t0 = millis();
        ok = strava_fetch(g_cfg.strava_server_url, g_fetch_result);
        Serial.printf("[Fetch] strava_fetch done in %lu ms, ok=%d, abort=%d\n",
                      millis() - t0, (int)ok, (int)g_fetch_abort);
        if (ok) break;
        if (attempt < FETCH_MAX_RETRIES && !g_fetch_abort) {
            Serial.printf("[Fetch] Failed, retrying in %u ms...\n", FETCH_RETRY_MS);
            // Sleep in small increments so we notice the abort flag promptly.
            for (uint32_t slept = 0; slept < FETCH_RETRY_MS && !g_fetch_abort; slept += 100)
                vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    __asm__ volatile("" ::: "memory");
    FetchState next = ok ? FETCH_DONE_OK : FETCH_DONE_FAIL;
    Serial.printf("[Fetch] Task ending: ok=%d abort=%d -> state=%s\n",
                  (int)ok, (int)g_fetch_abort, next == FETCH_DONE_OK ? "DONE_OK" : "DONE_FAIL");
    portENTER_CRITICAL(&g_fetch_task_mux);
    g_fetch_task = nullptr;
    portEXIT_CRITICAL(&g_fetch_task_mux);
    g_fetch_state = next;
    vTaskDelete(nullptr);
}

// Called from main loop — applies a completed fetch result to the display.
static void apply_fetch_result(bool ok) {
    Serial.printf("[Apply] ok=%d display_ready=%d valid=%d week_count=%d\n",
                  (int)ok, (int)g_display_ready,
                  (int)g_fetch_result.valid, (int)g_fetch_result.week_count);
    if (ok) {
        g_last_fetch_ms = millis();
        g_data = g_fetch_result;
        g_trend_refresh_pending = true;
        if (g_display_ready) {
            Serial.println("[Apply] Calling display_grid_update...");
            display_grid_update(GRID_METRIC_LOAD, g_data, g_cfg);
            Serial.println("[Apply] Load grid update done");
            display_grid_update(GRID_METRIC_CALORIES, g_data, g_cfg);
            Serial.println("[Apply] Calories grid update done");
            Serial.println("[Apply] Calling display_stats_update...");
            display_stats_update(g_data);
            Serial.println("[Apply] Stats update done");
            display_stats_set_age(0);
            Serial.println("[Apply] Age label update done");
        } else {
            Serial.println("[Apply] SKIPPED — display not ready");
        }
        refresh_screen_selection(true);
        // After refresh, the active screen may be a grid that needs animations started.
        // This also covers re-fetches where load_screen_index sees no screen change.
        if (g_screen_index == SCREEN_LOAD_GRID)
            display_grid_start_animations(GRID_METRIC_LOAD, g_data, g_cfg);
        else if (g_screen_index == SCREEN_CALORIES_GRID)
            display_grid_start_animations(GRID_METRIC_CALORIES, g_data, g_cfg);
        Serial.println("[Apply] D1");
    } else {
        Serial.println("[Fetch] All attempts failed — display not updated");
        refresh_screen_selection(false);
    }

    Serial.println("[Apply] D2");
    update_status_screen();
    Serial.println("[Apply] D3");
}

static void refresh_trend_screen_if_needed() {
    if (!g_screen_calorie_trend) {
        Serial.println("[Trend] Skip refresh - screen missing");
        return;
    }
    if (!g_data.valid) {
        Serial.println("[Trend] Skip refresh - no valid data");
        return;
    }

    Serial.printf("[Trend] Refresh start (pending=%d)\n", (int)g_trend_refresh_pending);
    display_calorie_trend_update(g_data);
    g_trend_refresh_pending = false;
    Serial.println("[Trend] Refresh done");
}

// Kick off an async fetch. If one is already running or WiFi is down, do nothing.
static void do_fetch() {
    if (g_fetch_state == FETCH_RUNNING) return;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Fetch] Skipping — WiFi not connected");
        update_status_screen();
        return;
    }
    display_grid_stop_animations(GRID_METRIC_LOAD);
    display_grid_stop_animations(GRID_METRIC_CALORIES);
    g_fetch_started_ms = millis();
    g_fetch_abort_ms   = 0;
    g_last_fetch_attempt_ms = millis();
    g_fetch_abort  = false;
    g_fetch_state  = FETCH_RUNNING;
    memset(&g_fetch_result, 0, sizeof(g_fetch_result));
    if (xTaskCreate(fetch_task, "strava_fetch", 16384, nullptr, 1, &g_fetch_task) != pdPASS) {
        Serial.println("[Fetch] Task create failed — heap exhausted");
        g_fetch_state = FETCH_IDLE;
    }

    update_status_screen();
}

static void refresh_screen_selection(bool prefer_primary) {
    uint8_t target = first_available_screen();
    if (has_valid_dataset() && prefer_primary && screen_is_available(SCREEN_LOAD_GRID)) {
        target = SCREEN_LOAD_GRID;
    }

    if ((!screen_is_available(g_screen_index)) || (g_screen_index != target && !has_valid_dataset())) {
        load_screen_index(target, false);
        return;
    }

    if (g_screen_switch_timer) {
        lv_timer_set_period(g_screen_switch_timer, screen_period_ms(g_screen_index));
        lv_timer_reset(g_screen_switch_timer);
    }
}

static void update_status_screen() {
    if (!g_screen_status) return;

    String title;
    String body;
    lv_color_t accent = lv_color_hex(0x4cc9f0);
    const char *wifi_ssid = g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : WIFI_SSID;
    const char *server_url = g_cfg.strava_server_url[0] ? g_cfg.strava_server_url : "not configured";

    if (WiFi.status() != WL_CONNECTED) {
        title = "Wi-Fi offline";
        accent = lv_color_hex(0xffb454);
        body.reserve(196);
        body += "Trying to reconnect to:\n";
        body += wifi_ssid;
        body += "\n\nStatus: ";
        body += wifi_status_text(WiFi.status());
        body += "\nIP: ";
        body += current_ip_string();
    } else if (g_fetch_state == FETCH_RUNNING && !has_valid_dataset()) {
        title = "Loading activity";
        accent = lv_color_hex(0x4cc9f0);
        body.reserve(220);
        body += "Wi-Fi: ";
        body += current_wifi_ssid_string();
        body += "\nIP: ";
        body += current_ip_string();
        body += "\n\nFetching from:\n";
        {
            String url_str(server_url);
            if (url_str.length() > 34) {
                body += url_str.substring(0, 31);
                body += "...";
            } else {
                body += url_str;
            }
        }
    } else if (!g_cfg.strava_server_url[0]) {
        title = "Server not set";
        accent = lv_color_hex(0xff8f70);
        body = "Open the device config page and add the Strava bridge URL.";
    } else {
        title = "Server unavailable";
        accent = lv_color_hex(0xff8f70);
        body.reserve(220);
        body += "Wi-Fi: ";
        body += current_wifi_ssid_string();
        body += "\nEndpoint:\n";
        String url_str(server_url);
        if (url_str.length() > 34) {
            body += url_str.substring(0, 31);
            body += "...";
        } else {
            body += url_str;
        }
        body += "\n\nNo payload received yet.";
    }

    display_status_update(title.c_str(), body.c_str(), accent);
}

static void status_screen_tick() {
    uint32_t now = millis();
    if (now - g_last_status_refresh_ms < 1000UL) return;
    g_last_status_refresh_ms = now;
    update_status_screen();
    refresh_screen_selection(false);
}

static void bootstrap_fetch_retry_tick() {
    static constexpr uint32_t kBootstrapRetryMs = 60000UL;

    if (has_valid_dataset()) return;
    if (g_fetch_state != FETCH_IDLE) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (g_last_fetch_attempt_ms != 0 && millis() - g_last_fetch_attempt_ms < kBootstrapRetryMs) return;
    do_fetch();
}

// Called every loop iteration — checks if the fetch task finished or timed out.
// The abort-drain window is non-blocking: we set a timestamp and return, checking
// each loop() call whether the task has exited. Do not force-delete the task
// while HTTP/lwIP calls are active; the fetch task must unwind its own socket
// state or lwIP can later signal a freed wait queue and assert.
static void fetch_tick() {
    if (g_fetch_state == FETCH_IDLE) return;

    if (g_fetch_state == FETCH_RUNNING) {
        // Non-blocking drain: if we already signalled abort, just wait for the
        // task to null its own handle after the current HTTP call times out.
        if (g_fetch_abort_ms != 0) {
            portENTER_CRITICAL(&g_fetch_task_mux);
            TaskHandle_t h = g_fetch_task;
            portEXIT_CRITICAL(&g_fetch_task_mux);

            if (h == nullptr) {
                return;
            }
            return;
        }

        if (millis() - g_fetch_started_ms >= FETCH_TIMEOUT_MS) {
            Serial.printf("[Fetch] Timeout after %lu ms — requesting abort\n",
                          millis() - g_fetch_started_ms);
            g_fetch_abort    = true;
            g_fetch_abort_ms = millis() | 1u;
        }
        return;
    }

    // Task finished normally
    g_fetch_started_ms = 0;
    g_fetch_abort_ms   = 0;
    bool ok = (g_fetch_state == FETCH_DONE_OK);
    g_fetch_state = FETCH_IDLE;
    apply_fetch_result(ok);
}

static void refresh_timer_cb(lv_timer_t *) {
    if (g_last_fetch_ms != 0) {
        uint32_t age_ms = millis() - g_last_fetch_ms;
        display_stats_set_age(age_ms / 60000UL);
    }
    do_fetch();
}

// ── WiFi connection splash ────────────────────────────────────────────────────

static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static constexpr uint8_t WIFI_CONNECT_MAX_RETRIES = 3;

static bool wifi_connect_splash() {
    style_active_screen_black();

    lv_obj_t *spinner = lv_spinner_create(lv_scr_act());
    lv_spinner_set_anim_params(spinner, 8000, 200);
    lv_obj_set_size(spinner, 80, 80);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x39d353), LV_PART_INDICATOR);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *lbl = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(lbl, ui_font_label(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x39d353), 0);
    lv_obj_set_width(lbl, 280);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    const char *ssid = g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : WIFI_SSID;
    const char *pass = g_cfg.wifi_password[0] ? g_cfg.wifi_password : WIFI_PASSWORD;

    lv_obj_align_to(lbl, spinner, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);

    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);

    bool connected = false;
    for (uint8_t attempt = 1; attempt <= WIFI_CONNECT_MAX_RETRIES; ++attempt) {
        lv_label_set_text_fmt(lbl, "Connecting to\n%s\nAttempt %u/%u",
                              ssid,
                              attempt,
                              WIFI_CONNECT_MAX_RETRIES);

        WiFi.disconnect(false, true);
        delay(200);
        WiFi.begin(ssid, pass);

        uint32_t wifi_start_ms = millis();
        while (WiFi.status() != WL_CONNECTED) {
            wl_status_t status = WiFi.status();
            lv_timer_handler();
            delay(50);

            if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
                Serial.printf("[WiFi] Connect attempt %u/%u failed (status=%d)\n",
                              attempt,
                              WIFI_CONNECT_MAX_RETRIES,
                              (int)status);
                break;
            }

            if (millis() - wifi_start_ms >= WIFI_CONNECT_TIMEOUT_MS) {
                Serial.printf("[WiFi] Connect attempt %u/%u timed out\n",
                              attempt,
                              WIFI_CONNECT_MAX_RETRIES);
                break;
            }
        }

        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
            break;
        }
    }

    if (!connected) {
        Serial.printf("[WiFi] Failed to connect after %u attempts\n", WIFI_CONNECT_MAX_RETRIES);
    }

    lv_obj_del(spinner);
    lv_obj_del(lbl);
    return connected;
}

// ── AP mode splash (first boot, no creds in NVS) ─────────────────────────────

static void ap_mode_splash(const char *ap_ssid) {
    style_active_screen_black();

    lv_obj_t *lbl = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(lbl, ui_font_label(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xd29922), 0);
    lv_obj_set_width(lbl, 300);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(lbl,
        "Setup mode\n"
        "Connect Wi-Fi to:\n"
        "%s\n"
        "Then open:\n"
        "http://192.168.4.1",
        ap_ssid);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

static void run_setup_ap_mode() {
    WiFi.disconnect(true, false);
    delay(100);
    WiFi.mode(WIFI_AP);

    bool ap_started = WiFi.softAP(kSetupApSsid, nullptr, 6, false, 4);
    if (!ap_started) {
        Serial.println("[WiFi] AP start failed");
    } else {
        Serial.printf("[WiFi] AP mode: %s @ %s\n",
                      kSetupApSsid,
                      WiFi.softAPIP().toString().c_str());
    }

    ap_mode_splash(kSetupApSsid);
    web_server_start(g_cfg, [](const Config &cfg) {
        gfx->setRotation(cfg.flip_screen ? 3 : 1);
        set_screen_brightness_pct(cfg.brightness);
    });

    while (true) {
        buttons_tick();
        web_server_handle();
        lv_timer_handler();
        delay(5);
    }
}

// ── LVGL init ────────────────────────────────────────────────────────────────

static void lvgl_init() {
    lv_init();
    lv_tick_set_cb(millis_cb);
#if LV_USE_LOG != 0
    lv_log_register_print_cb(my_log_print);
#endif
    uint32_t w = gfx->width();
    uint32_t h = gfx->height();
    uint32_t buf_lines = 24;
    uint32_t buf_size = w * buf_lines;

    g_lvgl_buf = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t),
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_lvgl_buf) {
        buf_lines = 12;
        buf_size = w * buf_lines;
        g_lvgl_buf = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t),
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!g_lvgl_buf) {
        g_lvgl_buf = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    }
    if (!g_lvgl_buf) { Serial.println("LVGL buf alloc failed!"); while (true); }

    disp = lv_display_create(w, h);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, g_lvgl_buf, NULL, buf_size * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    style_active_screen_black();
}

// ── setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    DEV_DEVICE_INIT();
    pinMode(BTN_A, INPUT_PULLUP);
    pinMode(BTN_B, INPUT_PULLUP);
    delay(2000);

    if (!gfx->begin()) { Serial.println("GFX init failed!"); while (true); }
    gfx->setRotation(1); // landscape: 320x170 (pre-config default)
    gfx->fillScreen(RGB565_BLACK);
    set_screen_brightness_pct(100); // full brightness during splash

    Serial.printf("[Heap] Before fonts: free=%lu largest=%lu\n",
                  (unsigned long)esp_get_free_heap_size(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    lvgl_init();
    ui_fonts_init();
    Serial.printf("[Heap] After fonts:  free=%lu largest=%lu\n",
                  (unsigned long)esp_get_free_heap_size(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    config_load(g_cfg);
    gfx->setRotation(g_cfg.flip_screen ? 3 : 1); // apply saved orientation
    set_screen_brightness_pct(g_cfg.brightness);

    if (g_cfg.wifi_ssid[0] == '\0') {
        run_setup_ap_mode();
    }

    bool wifi_connected = wifi_connect_splash();
    if (!wifi_connected) {
        Serial.println("[WiFi] Continuing boot without an active STA connection");
    }

    web_server_start(g_cfg, [](const Config &cfg) {
        gfx->setRotation(cfg.flip_screen ? 3 : 1);
        set_screen_brightness_pct(cfg.brightness);
        mqtt_client_reinit();
        if (g_display_ready && g_data.valid) {
            display_grid_update(GRID_METRIC_LOAD, g_data, cfg);
            display_grid_update(GRID_METRIC_CALORIES, g_data, cfg);
            g_trend_refresh_pending = true;
        }
    });
    mqtt_client_init(g_cfg);

    g_screen_status        = display_status_build();
    g_screen_grid          = display_grid_build(GRID_METRIC_LOAD, "Exercise Load", g_cfg);
    g_screen_stats         = display_stats_build("Exercise Load");
    g_screen_calories      = display_grid_build(GRID_METRIC_CALORIES, "Calories Burned", g_cfg);
    g_screen_calorie_trend = display_calorie_trend_build("7 Day Burn");
    g_display_ready = (g_screen_status != nullptr || g_screen_grid != nullptr || g_screen_stats != nullptr ||
                       g_screen_calories != nullptr || g_screen_calorie_trend != nullptr);
    g_screen_index = SCREEN_STATUS;

    update_status_screen();
    load_screen_index(first_available_screen(), false);

    if (g_display_ready) {
        do_fetch();
    }

    // Load grid is shown first, so start with its longer interval.
    g_screen_switch_timer = lv_timer_create(screen_switch_cb,
                    screen_period_ms(g_screen_index), nullptr);
    lv_timer_create(refresh_timer_cb,
                    (uint32_t)g_cfg.refresh_interval_min * 60UL * 1000UL, nullptr);

    Serial.println("[Main] Setup complete");
}

// ── WiFi watchdog ─────────────────────────────────────────────────────────────
// The ESP32 WiFi driver auto-reconnects, but it can silently stall after a
// prolonged outage. This watchdog calls WiFi.reconnect() if the link has been
// down for more than WIFI_RECONNECT_MS, which kicks the driver without a reboot.

static const uint32_t WIFI_RECONNECT_MS  = 15000; // 15 s before forcing reconnect
static const uint32_t WIFI_HARD_RESET_MS = 60000; // 60 s before full disconnect+reconnect
static uint32_t g_wifi_lost_ms = 0;
static uint32_t g_wifi_reconnect_count = 0;

static void wifi_watchdog_tick() {
    if (WiFi.status() == WL_CONNECTED) {
        if (g_wifi_lost_ms != 0) {
            Serial.printf("[WiFi] Reconnected after %lu ms (attempt #%lu)\n",
                          millis() - g_wifi_lost_ms, g_wifi_reconnect_count);
            g_wifi_lost_ms = 0;
            g_wifi_reconnect_count = 0;
            g_last_fetch_attempt_ms = 0;
            do_fetch();
        }
        return;
    }
    uint32_t now = millis();
    if (g_wifi_lost_ms == 0) {
        g_wifi_lost_ms = now;
        Serial.println("[WiFi] Lost connection");
        return;
    }
    uint32_t lost_ms = now - g_wifi_lost_ms;

    // Don't touch the radio while a fetch task is active — the fetch has its
    // own timeout and will fail cleanly; we reconnect after it finishes.
    if (g_fetch_state == FETCH_RUNNING) return;

    if (lost_ms >= WIFI_HARD_RESET_MS && g_wifi_reconnect_count >= 2) {
        // Full teardown+reconnect after multiple soft-reconnect attempts fail
        Serial.println("[WiFi] Hard reset — disconnect + begin");
        const char *ssid = g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : WIFI_SSID;
        const char *pass = g_cfg.wifi_password[0] ? g_cfg.wifi_password : WIFI_PASSWORD;
        WiFi.disconnect(false, true);
        delay(200);
        WiFi.begin(ssid, pass);
        g_wifi_lost_ms = now;
        g_wifi_reconnect_count = 0;
    } else if (lost_ms >= WIFI_RECONNECT_MS) {
        Serial.printf("[WiFi] Soft reconnect (attempt #%lu)\n", g_wifi_reconnect_count + 1);
        WiFi.reconnect();
        g_wifi_lost_ms = now;
        g_wifi_reconnect_count++;
    }
}

// ── loop ──────────────────────────────────────────────────────────────────────

void loop() {
    buttons_tick();
    wifi_watchdog_tick();
    fetch_tick();
    bootstrap_fetch_retry_tick();
    status_screen_tick();
    lv_timer_handler();
    web_server_handle();
    mqtt_client_tick();
    delay(5);
}
