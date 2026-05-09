#pragma once
#include <Arduino.h>

inline constexpr char kSetupApSsid[] = "StravaMonitor-Setup";
inline constexpr uint8_t kMinHistoryMonths = 1;
inline constexpr uint8_t kDefaultHistoryMonths = 6;
inline constexpr uint8_t kMaxHistoryMonths = 12;

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
    uint8_t  history_months;        // months of history to show on the grid (default 6)
    uint8_t  anim_top_pct;          // animate top N % of active days (default 20)
    uint16_t anim_period_ms;        // breathing animation period ms (default 2000)
    uint8_t  flip_screen;           // 0 = normal, 1 = 180 deg rotated

    // ── MQTT brightness control ───────────────────────────────────────────────
    char     mqtt_broker[64];
    uint16_t mqtt_port;
    char     mqtt_lcd_topic[128];            // LCD brightness topic (0-100)
};

void config_load(Config &cfg);
void config_save(const Config &cfg);
void config_apply_defaults(Config &cfg);
void config_reset();   // clears all NVS preferences, forcing initial setup on next boot
