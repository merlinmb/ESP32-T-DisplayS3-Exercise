// Strava Exercise Load Monitor — TFT_eSPI + OneButton refactor
// LilyGo T-Display S3, landscape 320×170

#include <WiFi.h>
#include <Arduino.h>
#include <OneButton.h>
#include "PINS_T_DISPLAY_S3.h"
#include "secrets.h"
#include "config.h"
#include "strava_api.h"
#include "display.h"
#include "display_calorie_trend.h"
#include "display_grid.h"
#include "display_stats.h"
#include "display_status.h"
#include "web_server.h"
#include "mqtt_client.h"

// ── Screen indices ─────────────────────────────────────────────────────────────
enum ScreenIndex {
    SCREEN_STATUS = 0,
    SCREEN_LOAD_GRID,
    SCREEN_STATS,
    SCREEN_CALORIES_GRID,
    SCREEN_CALORIE_TREND,
    SCREEN_COUNT,
};

// ── Globals ───────────────────────────────────────────────────────────────────
static Config     g_cfg;
static StravaData g_data;
static uint8_t    g_screen_index  = SCREEN_STATUS;
static uint32_t   g_last_fetch_ms = 0;
static uint32_t   g_last_fetch_attempt_ms = 0;
static uint32_t   g_carousel_ms   = 0;
static uint32_t   g_refresh_timer_ms = 0;
static bool       g_trend_refresh_pending = true;

// Info overlay
static bool       g_info_visible  = false;
static uint32_t   g_info_shown_ms = 0;

// Breathing animation (grid screens)
static float      g_anim_phase    = 0.0f;
static uint32_t   g_anim_last_ms  = 0;

// OneButton instances (active-LOW, internal pull-up)
static OneButton  btn_a(BTN_A, true, true);
static OneButton  btn_b(BTN_B, true, true);

// Forward declarations
static void render_screen(uint8_t idx);
static void do_fetch();
static void carousel_reset();
static bool screen_is_available(uint8_t idx);
static uint8_t first_available_screen();
static bool select_next_available_screen(uint8_t current, uint8_t &next);
static void advance_screen();
static void toggle_overlay();
static void ip_to_cstr(const IPAddress &ip, char *out, size_t out_len);

// ── Display brightness ────────────────────────────────────────────────────────
// 16-step pulse-counting driver on GPIO38. NOT PWM — do not use ledcWrite.
static void apply_brightness(uint8_t level) {
    static bool    backlight_initialized = false;
    static uint8_t current_level = 0;
    static constexpr uint8_t kBacklightSteps = 16;

    if (level > kBacklightSteps) level = kBacklightSteps;

    if (!backlight_initialized) {
        pinMode(GFX_BL, OUTPUT);
        digitalWrite(GFX_BL, LOW);
        backlight_initialized = true;
    }

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

    int from   = kBacklightSteps - current_level;
    int to     = kBacklightSteps - level;
    int pulses = (kBacklightSteps + to - from) % kBacklightSteps;
    for (int i = 0; i < pulses; ++i) {
        digitalWrite(GFX_BL, LOW);
        digitalWrite(GFX_BL, HIGH);
    }
    current_level = level;
}

void set_screen_brightness_pct(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint8_t level = 0;
    if (pct != 0) {
        level = (uint8_t)((pct * 16UL + 99UL) / 100UL);
        if (level == 0) level = 1;
    }
    apply_brightness(level);
}

// ── Async data fetch via FreeRTOS task ────────────────────────────────────────
static const uint32_t FETCH_TIMEOUT_MS     = 120000;
static const uint32_t FETCH_ABORT_GRACE_MS = 45000;
static const int      FETCH_MAX_RETRIES    = 3;
static const uint32_t FETCH_RETRY_MS       = 5000;

enum FetchState { FETCH_IDLE, FETCH_RUNNING, FETCH_DONE_OK, FETCH_DONE_FAIL };
static volatile FetchState g_fetch_state      = FETCH_IDLE;
static volatile bool       g_fetch_abort      = false;
static StravaData           g_fetch_result;
static TaskHandle_t         g_fetch_task      = nullptr;
static portMUX_TYPE         g_fetch_task_mux  = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t    g_fetch_started_ms = 0;
static volatile uint32_t    g_fetch_abort_ms   = 0;

static void fetch_task(void *) {
    bool ok = false;
    for (int attempt = 1; attempt <= FETCH_MAX_RETRIES && !g_fetch_abort; attempt++) {
        Serial.printf("[Fetch] Attempt %d/%d (abort=%d)\n",
                      attempt, FETCH_MAX_RETRIES, (int)g_fetch_abort);
        ok = strava_fetch(g_cfg.strava_server_url, g_fetch_result);
        Serial.printf("[Fetch] ok=%d abort=%d\n", (int)ok, (int)g_fetch_abort);
        if (ok) break;
        if (attempt < FETCH_MAX_RETRIES && !g_fetch_abort) {
            for (uint32_t slept = 0; slept < FETCH_RETRY_MS && !g_fetch_abort; slept += 100)
                vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    __asm__ volatile("" ::: "memory");
    FetchState next = ok ? FETCH_DONE_OK : FETCH_DONE_FAIL;
    portENTER_CRITICAL(&g_fetch_task_mux);
    g_fetch_task = nullptr;
    portEXIT_CRITICAL(&g_fetch_task_mux);
    g_fetch_state = next;
    vTaskDelete(nullptr);
}

static void do_fetch() {
    if (g_fetch_state == FETCH_RUNNING) return;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Fetch] Skipping — WiFi not connected");
        render_screen(g_screen_index);
        return;
    }
    g_fetch_started_ms      = millis();
    g_fetch_abort_ms        = 0;
    g_last_fetch_attempt_ms = millis();
    g_fetch_abort           = false;
    g_fetch_state           = FETCH_RUNNING;
    memset(&g_fetch_result, 0, sizeof(g_fetch_result));
    if (xTaskCreate(fetch_task, "strava_fetch", 16384, nullptr, 1, &g_fetch_task) != pdPASS) {
        Serial.println("[Fetch] Task create failed — heap exhausted");
        g_fetch_state = FETCH_IDLE;
    }
    render_screen(g_screen_index);
}

static void fetch_tick() {
    if (g_fetch_state == FETCH_IDLE) return;

    if (g_fetch_state == FETCH_RUNNING) {
        if (g_fetch_abort_ms != 0) {
            portENTER_CRITICAL(&g_fetch_task_mux);
            TaskHandle_t h = g_fetch_task;
            portEXIT_CRITICAL(&g_fetch_task_mux);
            if (h != nullptr && millis() - g_fetch_abort_ms > FETCH_ABORT_GRACE_MS) {
                Serial.println("[Fetch] Abort drain exceeded; restarting device");
                delay(50);
                ESP.restart();
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

    // Task finished (DONE_OK or DONE_FAIL)
    g_fetch_started_ms = 0;
    g_fetch_abort_ms   = 0;
    bool ok = (g_fetch_state == FETCH_DONE_OK);
    g_fetch_state = FETCH_IDLE;

    if (ok) {
        g_last_fetch_ms        = millis();
        g_data                 = g_fetch_result;
        g_trend_refresh_pending = true;
        Serial.println("[Fetch] Data applied");
    } else {
        Serial.println("[Fetch] All attempts failed");
    }

    // Choose best screen to display
    if (g_data.valid) {
        if (!screen_is_available(g_screen_index) || g_screen_index == SCREEN_STATUS)
            g_screen_index = screen_is_available(SCREEN_LOAD_GRID)
                             ? SCREEN_LOAD_GRID : first_available_screen();
    }
    render_screen(g_screen_index);
    carousel_reset();
}

// ── WiFi watchdog ─────────────────────────────────────────────────────────────
static const uint32_t WIFI_RECONNECT_MS  = 15000;
static const uint32_t WIFI_HARD_RESET_MS = 60000;
static uint32_t       g_wifi_lost_ms     = 0;
static uint32_t       g_wifi_reconnect_count = 0;

static void wifi_watchdog_tick() {
    if (WiFi.status() == WL_CONNECTED) {
        if (g_wifi_lost_ms != 0) {
            Serial.printf("[WiFi] Reconnected after %lu ms (attempt #%lu)\n",
                          millis() - g_wifi_lost_ms, g_wifi_reconnect_count);
            g_wifi_lost_ms         = 0;
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
    if (g_fetch_state == FETCH_RUNNING) return;  // don't touch radio during fetch

    uint32_t lost_ms = now - g_wifi_lost_ms;
    if (lost_ms >= WIFI_HARD_RESET_MS && g_wifi_reconnect_count >= 2) {
        const char *ssid = g_cfg.wifi_ssid[0]     ? g_cfg.wifi_ssid     : WIFI_SSID;
        const char *pass = g_cfg.wifi_password[0] ? g_cfg.wifi_password : WIFI_PASSWORD;
        Serial.println("[WiFi] Hard reset — disconnect + begin");
        WiFi.disconnect(false, true);
        delay(200);
        WiFi.begin(ssid, pass);
        g_wifi_lost_ms         = now;
        g_wifi_reconnect_count = 0;
    } else if (lost_ms >= WIFI_RECONNECT_MS) {
        Serial.printf("[WiFi] Soft reconnect (attempt #%lu)\n", g_wifi_reconnect_count + 1);
        WiFi.reconnect();
        g_wifi_lost_ms = now;
        g_wifi_reconnect_count++;
    }
}

// ── Screen availability ───────────────────────────────────────────────────────
static bool screen_is_available(uint8_t idx) {
    switch (idx) {
        case SCREEN_STATUS:
            return !g_data.valid;
        case SCREEN_LOAD_GRID:
        case SCREEN_STATS:
        case SCREEN_CALORIES_GRID:
        case SCREEN_CALORIE_TREND:
            return g_data.valid;
        default:
            return false;
    }
}

static uint8_t first_available_screen() {
    for (uint8_t i = 0; i < SCREEN_COUNT; i++)
        if (screen_is_available(i)) return i;
    return SCREEN_STATUS;
}

static bool select_next_available_screen(uint8_t current, uint8_t &next) {
    for (int step = 0; step < (int)SCREEN_COUNT; step++) {
        uint8_t candidate = (uint8_t)((current + 1 + step) % SCREEN_COUNT);
        if (screen_is_available(candidate)) {
            next = candidate;
            return true;
        }
    }
    return false;
}

// ── Carousel ──────────────────────────────────────────────────────────────────
static void carousel_reset() { g_carousel_ms = millis(); }

static void carousel_tick() {
    uint16_t secs = g_cfg.screen_switch_secs ? g_cfg.screen_switch_secs : 5;
    if (millis() - g_carousel_ms < (uint32_t)secs * 1000UL) return;
    uint8_t next = 0;
    if (select_next_available_screen(g_screen_index, next)) {
        g_screen_index = next;
        render_screen(g_screen_index);
    }
    carousel_reset();
}

// ── Overlay ───────────────────────────────────────────────────────────────────
static bool wifi_ap_mode_active() {
    wifi_mode_t mode = WiFi.getMode();
    return mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
}

static void ip_to_cstr(const IPAddress &ip, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static void build_network_overlay_text(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    char ip_buf[20]   = "not connected";
    char ssid_buf[40] = "not connected";
    const bool ap_active = wifi_ap_mode_active();
    if (ap_active) {
        ip_to_cstr(WiFi.softAPIP(), ip_buf, sizeof(ip_buf));
    } else if (WiFi.status() == WL_CONNECTED) {
        ip_to_cstr(WiFi.localIP(), ip_buf, sizeof(ip_buf));
        WiFi.SSID().toCharArray(ssid_buf, sizeof(ssid_buf));
        if (!ssid_buf[0]) strncpy(ssid_buf, "connected", sizeof(ssid_buf) - 1);
    }
    const char *ap_text = ap_active ? kSetupApSsid : "off";
    snprintf(out, out_len, "IP: %s\nWi-Fi: %s\nAP: %s", ip_buf, ssid_buf, ap_text);
}

static void draw_info_overlay(TFT_eSprite &spr) {
    char text[160];
    build_network_overlay_text(text, sizeof(text));
    const int ox = (DISP_W - 220) / 2;
    const int oy = 10;
    spr.fillRoundRect(ox, oy, 220, 82, 8, RGB565(0x0D, 0x1B, 0x26));
    spr.drawRoundRect(ox, oy, 220, 82, 8, RGB565(0x39, 0xD3, 0x53));
    draw_multiline(spr, text, ox + 8, oy + 8, TFT_WHITE, 2, 20);
}

static void toggle_overlay() {
    g_info_visible = !g_info_visible;
    if (g_info_visible) g_info_shown_ms = millis() | 1u;
    render_screen(g_screen_index);
}

static void overlay_tick() {
    if (!g_info_visible) return;
    if (millis() - g_info_shown_ms >= 4000UL) {
        g_info_visible = false;
        render_screen(g_screen_index);
    }
}

// ── Status screen text builder ────────────────────────────────────────────────
static void build_status_content(char *title, size_t title_sz,
                                  char *body, size_t body_sz,
                                  uint16_t &accent) {
    const char *wifi_ssid  = g_cfg.wifi_ssid[0]           ? g_cfg.wifi_ssid           : WIFI_SSID;
    const char *server_url = g_cfg.strava_server_url[0]   ? g_cfg.strava_server_url   : "";

    auto short_url = [&](char *dst, size_t dst_sz) {
        if (!server_url[0]) {
            strncpy(dst, "not configured", dst_sz - 1); dst[dst_sz - 1] = '\0';
        } else if (strlen(server_url) > 34) {
            memcpy(dst, server_url, 31); dst[31]='.'; dst[32]='.'; dst[33]='.'; dst[34]='\0';
        } else {
            strncpy(dst, server_url, dst_sz - 1); dst[dst_sz - 1] = '\0';
        }
    };

    if (WiFi.status() != WL_CONNECTED) {
        strncpy(title, "Wi-Fi offline", title_sz - 1);
        accent = C_AMBER;
        char ip[20]; ip_to_cstr(WiFi.localIP(), ip, sizeof(ip));
        snprintf(body, body_sz, "Trying to reconnect to:\n%s\n\nStatus: %d\nIP: %s",
                 wifi_ssid, (int)WiFi.status(), ip);
    } else if (g_fetch_state == FETCH_RUNNING && !g_data.valid) {
        strncpy(title, "Loading activity", title_sz - 1);
        accent = C_CYAN;
        char ip[20]; ip_to_cstr(WiFi.localIP(), ip, sizeof(ip));
        char url_s[36]; short_url(url_s, sizeof(url_s));
        snprintf(body, body_sz, "Wi-Fi: %s\nIP: %s\n\nFetching from:\n%s",
                 WiFi.SSID().c_str(), ip, url_s);
    } else if (!server_url[0]) {
        strncpy(title, "Server not set", title_sz - 1);
        accent = C_RED_ERR;
        strncpy(body, "Open the device config page and add the Strava bridge URL.", body_sz - 1);
    } else {
        strncpy(title, "Server unavailable", title_sz - 1);
        accent = C_RED_ERR;
        char url_s[36]; short_url(url_s, sizeof(url_s));
        snprintf(body, body_sz, "Wi-Fi: %s\nEndpoint:\n%s\n\nNo payload received yet.",
                 WiFi.SSID().c_str(), url_s);
    }
    title[title_sz - 1] = '\0';
    body[body_sz - 1]   = '\0';
}

// ── Central render dispatch ───────────────────────────────────────────────────
static uint32_t age_minutes() {
    if (g_last_fetch_ms == 0) return 0;
    return (millis() - g_last_fetch_ms) / 60000UL;
}

static void render_screen(uint8_t idx) {
    g_sprite.fillSprite(TFT_BLACK);
    switch (idx) {
        case SCREEN_STATUS: {
            char title[64] = "";
            char body[220] = "";
            uint16_t accent = C_CYAN;
            build_status_content(title, sizeof(title), body, sizeof(body), accent);
            display_status_render(g_sprite, title, body, accent);
            break;
        }
        case SCREEN_LOAD_GRID:
            display_grid_render(g_sprite, g_data, false, g_anim_phase, g_cfg);
            break;
        case SCREEN_STATS:
            display_stats_render(g_sprite, g_data, age_minutes());
            break;
        case SCREEN_CALORIES_GRID:
            display_grid_render(g_sprite, g_data, true, g_anim_phase, g_cfg);
            break;
        case SCREEN_CALORIE_TREND:
            display_calorie_trend_render(g_sprite, g_data);
            break;
        default:
            break;
    }
    if (g_info_visible) draw_info_overlay(g_sprite);
    display_push();
}

// ── Advance screen (button A click) ──────────────────────────────────────────
static void advance_screen() {
    uint8_t next = 0;
    if (!select_next_available_screen(g_screen_index, next)) return;
    g_screen_index = next;
    render_screen(g_screen_index);
    carousel_reset();
}

// ── Animation tick (10 fps breathing on grid screens) ─────────────────────────
static void anim_tick() {
    const bool on_grid = (g_screen_index == SCREEN_LOAD_GRID ||
                          g_screen_index == SCREEN_CALORIES_GRID);
    if (!on_grid) return;
    if (millis() - g_anim_last_ms < 100) return;
    g_anim_last_ms = millis();
    g_anim_phase   = (sinf((float)millis() * 0.001f) + 1.0f) * 0.5f;
    render_screen(g_screen_index);
}

// ── Status screen tick ────────────────────────────────────────────────────────
static void status_screen_tick() {
    static uint32_t s_last_ms = 0;
    if (millis() - s_last_ms < 1000UL) return;
    s_last_ms = millis();
    if (g_screen_index == SCREEN_STATUS && g_data.valid) {
        g_screen_index = screen_is_available(SCREEN_LOAD_GRID)
                         ? SCREEN_LOAD_GRID : first_available_screen();
        render_screen(g_screen_index);
        carousel_reset();
    } else if (g_screen_index == SCREEN_STATUS) {
        render_screen(SCREEN_STATUS);  // refresh text (WiFi status, etc.)
    }
}

// ── Bootstrap fetch retry ─────────────────────────────────────────────────────
static void bootstrap_fetch_retry_tick() {
    static constexpr uint32_t kBootstrapRetryMs = 60000UL;
    if (g_data.valid)                    return;
    if (g_fetch_state != FETCH_IDLE)     return;
    if (WiFi.status() != WL_CONNECTED)   return;
    if (g_last_fetch_attempt_ms != 0 &&
        millis() - g_last_fetch_attempt_ms < kBootstrapRetryMs) return;
    do_fetch();
}

// ── Periodic data refresh timer ───────────────────────────────────────────────
static void refresh_timer_tick() {
    if (g_refresh_timer_ms == 0) {
        g_refresh_timer_ms = millis();
        return;
    }
    uint32_t interval_ms = (uint32_t)(g_cfg.refresh_interval_min
                                      ? g_cfg.refresh_interval_min : 15)
                           * 60000UL;
    if (millis() - g_refresh_timer_ms < interval_ms) return;
    g_refresh_timer_ms = millis();
    do_fetch();
}

// ── WiFi connect splash ────────────────────────────────────────────────────────
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
static constexpr uint8_t  WIFI_CONNECT_MAX_RETRIES = 3;

static void draw_wifi_splash(const char *ssid, uint8_t attempt) {
    g_sprite.fillSprite(TFT_BLACK);
    char body[120];
    snprintf(body, sizeof(body), "Joining network:\n%s\n\nAttempt %u of %u",
             ssid, (unsigned)attempt, (unsigned)WIFI_CONNECT_MAX_RETRIES);
    display_status_render(g_sprite, "Connecting...", body, C_CYAN);
    display_push();
}

static bool wifi_connect_splash() {
    const char *ssid = g_cfg.wifi_ssid[0]     ? g_cfg.wifi_ssid     : WIFI_SSID;
    const char *pass = g_cfg.wifi_password[0] ? g_cfg.wifi_password : WIFI_PASSWORD;
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);

    bool connected = false;
    for (uint8_t attempt = 1; attempt <= WIFI_CONNECT_MAX_RETRIES; ++attempt) {
        Serial.printf("[WiFi] Connect attempt %u/%u\n", attempt, WIFI_CONNECT_MAX_RETRIES);
        draw_wifi_splash(ssid, attempt);
        if (attempt > 1) {
            WiFi.mode(WIFI_MODE_NULL);
            delay(120);
            WiFi.mode(WIFI_STA);
            delay(120);
        }
        WiFi.begin(ssid, pass);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED) {
            wl_status_t status = WiFi.status();
            delay(50);
            if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) break;
            if (millis() - t0 >= WIFI_CONNECT_TIMEOUT_MS) break;
        }
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            Serial.printf("[WiFi] Connected on attempt %u, IP: %s\n",
                          attempt, WiFi.localIP().toString().c_str());
            break;
        }
    }
    return connected;
}

// ── AP mode ───────────────────────────────────────────────────────────────────
static void ap_mode_splash(const char *ap_ssid) {
    g_sprite.fillSprite(TFT_BLACK);
    char msg[140];
    snprintf(msg, sizeof(msg),
             "Setup mode\nConnect Wi-Fi to:\n%s\nThen open:\nhttp://192.168.4.1",
             ap_ssid);
    draw_multiline(g_sprite, msg, 10, 15, RGB565(0xD2, 0x99, 0x22), 2, 20);
    display_push();
}

static void run_setup_ap_mode() {
    WiFi.mode(WIFI_MODE_NULL);
    delay(100);
    WiFi.mode(WIFI_AP);
    bool ap_started = WiFi.softAP(kSetupApSsid, nullptr, 6, false, 4);
    if (!ap_started) {
        Serial.println("[WiFi] AP start failed");
    } else {
        Serial.printf("[WiFi] AP mode: %s @ %s\n",
                      kSetupApSsid, WiFi.softAPIP().toString().c_str());
    }
    ap_mode_splash(kSetupApSsid);
    web_server_start(g_cfg, [](const Config &cfg) {
        g_tft.setRotation(cfg.flip_screen ? 3 : 1);
        set_screen_brightness_pct(cfg.brightness);
    });
    while (true) {
        btn_a.tick();
        btn_b.tick();
        web_server_handle();
        mqtt_client_tick();
        delay(5);
    }
}

// ── Button callbacks ──────────────────────────────────────────────────────────
static void on_btn_a_click()       { advance_screen(); }
static void on_btn_a_long_press()  {
    Serial.println("[Buttons] A long press — rebooting");
    delay(50); ESP.restart();
}
static void on_btn_b_click()       { toggle_overlay(); }
static void on_btn_b_long_press()  {
    Serial.println("[Buttons] B long press — rebooting");
    delay(50); ESP.restart();
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    config_load(g_cfg);
    Serial.printf("[Main] Loaded config: brightness=%u flip=%u\n",
                  g_cfg.brightness, g_cfg.flip_screen);

    display_init(g_cfg.flip_screen);
    set_screen_brightness_pct(100);

    // Attach button callbacks
    btn_a.attachClick(on_btn_a_click);
    btn_a.attachLongPressStart(on_btn_a_long_press);
    btn_b.attachClick(on_btn_b_click);
    btn_b.attachLongPressStart(on_btn_b_long_press);

    // Apply saved brightness (0 → fail-safe 35%)
    if (g_cfg.brightness == 0)
        set_screen_brightness_pct(35);
    else
        set_screen_brightness_pct(g_cfg.brightness);

    render_screen(SCREEN_STATUS);

    if (g_cfg.wifi_ssid[0] == '\0') {
        run_setup_ap_mode();  // does not return
    }

    bool wifi_connected = wifi_connect_splash();
    if (!wifi_connected) {
        Serial.printf("[WiFi] No connection after %u attempts; rebooting\n",
                      WIFI_CONNECT_MAX_RETRIES);
        delay(1000);
        ESP.restart();
    }

    web_server_start(g_cfg, [](const Config &cfg) {
        g_tft.setRotation(cfg.flip_screen ? 3 : 1);
        set_screen_brightness_pct(cfg.brightness);
        mqtt_client_reinit();
        carousel_reset();
    });
    mqtt_client_init(g_cfg);

    render_screen(SCREEN_STATUS);
    carousel_reset();
    do_fetch();
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    btn_a.tick();
    btn_b.tick();
    overlay_tick();
    carousel_tick();
    anim_tick();
    wifi_watchdog_tick();
    fetch_tick();
    bootstrap_fetch_retry_tick();
    refresh_timer_tick();
    status_screen_tick();
    web_server_handle();
    mqtt_client_tick();

    // Heartbeat
    static uint32_t s_heartbeat_ms = 0;
    if (millis() - s_heartbeat_ms >= 5000UL) {
        s_heartbeat_ms = millis();
        Serial.printf("[Loop] alive wifi=%d free=%lu\n",
                      (int)WiFi.status(),
                      (unsigned long)esp_get_free_heap_size());
    }

    delay(5);
}

