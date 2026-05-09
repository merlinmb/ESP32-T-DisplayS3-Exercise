#pragma once
#include <Arduino.h>

inline constexpr char kSetupApSsid[] = "StravaMonitor-Setup";

struct Config {
    // ── Network ───────────────────────────────────────────────────────────────
    char     wifi_ssid[64];
    char     wifi_password[64];

    // ── Strava bridge ─────────────────────────────────────────────────────────
    // Full URL to the strava_bridge.py endpoint, e.g.:
    //   http://192.168.1.54:8082/api/exercise-load
    char     strava_server_url[160];

    // ── Display ───────────────────────────────────────────────────────────────
    uint8_t  brightness;            // LCD backlight 0-100 % (default 100)
    uint16_t screen_switch_secs;    // seconds between grid/stats screens (default 30)
    uint16_t refresh_interval_min;  // data refresh period in minutes (default 30)
    uint8_t  anim_top_pct;          // animate top N % of active days (default 20)
    uint16_t anim_period_ms;        // breathing animation period ms (default 2000)
    uint8_t  flip_screen;           // 0 = normal, 1 = 180 deg rotated

    // ── RGB LED (optional external NeoPixel) ──────────────────────────────────
    uint8_t  rgb_brightness;        // 0-100 % (default 100)
    uint16_t rgb_period_min_ms;     // fastest breath period (default 1200)
    uint16_t rgb_period_max_ms;     // slowest breath period (default 8000)
    uint8_t  rgb_streak_max;        // load minutes that achieves min period (default 150)

    // ── MQTT brightness control ───────────────────────────────────────────────
    char     mqtt_broker[64];
    uint16_t mqtt_port;
    char     mqtt_combined_topic[128];       // sets LCD + LED brightness (0-100)
    char     mqtt_lcd_topic[128];            // LCD brightness only
    char     mqtt_led_brightness_topic[128]; // LED brightness only
};

void config_load(Config &cfg);
void config_save(const Config &cfg);
void config_apply_defaults(Config &cfg);
void config_reset();   // clears all NVS preferences, forcing initial setup on next boot
