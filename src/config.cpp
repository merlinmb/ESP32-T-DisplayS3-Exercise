#include "config.h"
#include <Preferences.h>

static Preferences prefs;

void config_apply_defaults(Config &cfg) {
    // Clamp brightness: 0 is a valid off state, values >100 reset to 100 %.
    if (cfg.brightness > 100) cfg.brightness = 100;
    if (cfg.screen_switch_secs == 0)   cfg.screen_switch_secs   = 30;
    if (cfg.refresh_interval_min == 0) cfg.refresh_interval_min = 30;
    if (cfg.history_months < kMinHistoryMonths || cfg.history_months > kMaxHistoryMonths)
        cfg.history_months = kDefaultHistoryMonths;
    if (cfg.anim_top_pct == 0)         cfg.anim_top_pct         = 20;
    if (cfg.anim_period_ms == 0)       cfg.anim_period_ms       = 2000;
    if (cfg.mqtt_port == 0)            cfg.mqtt_port            = 1883;
    if (cfg.mqtt_lcd_topic[0] == '\0')
        strncpy(cfg.mqtt_lcd_topic, "cmnd/mcmddevices/lcdbrightness",
                sizeof(cfg.mqtt_lcd_topic) - 1);
}

void config_load(Config &cfg) {
    memset(&cfg, 0, sizeof(cfg));
    prefs.begin("stravamon", false);
    prefs.getString("wifi_ssid",   cfg.wifi_ssid,          sizeof(cfg.wifi_ssid));
    prefs.getString("wifi_pass",   cfg.wifi_password,      sizeof(cfg.wifi_password));
    prefs.getString("srv_url",     cfg.strava_server_url,  sizeof(cfg.strava_server_url));
    cfg.brightness           = prefs.getUChar( "brightness",  255);
    cfg.screen_switch_secs   = prefs.getUShort("switch_sec",  0);
    cfg.refresh_interval_min = prefs.getUShort("refresh_min", 0);
    cfg.history_months       = prefs.getUChar( "hist_months", 0);
    cfg.anim_top_pct         = prefs.getUChar( "anim_pct",    0);
    cfg.anim_period_ms       = prefs.getUShort("anim_ms",     0);
    cfg.flip_screen          = prefs.getUChar( "flip_scr",    0);
    prefs.getString("mqtt_host",   cfg.mqtt_broker,              sizeof(cfg.mqtt_broker));
    cfg.mqtt_port            = prefs.getUShort("mqtt_port",   0);
    prefs.getString("mqtt_lcd",    cfg.mqtt_lcd_topic,           sizeof(cfg.mqtt_lcd_topic));
    if (cfg.mqtt_lcd_topic[0] == '\0') {
        prefs.getString("mqtt_ctopic", cfg.mqtt_lcd_topic, sizeof(cfg.mqtt_lcd_topic));
    }
    prefs.end();
    config_apply_defaults(cfg);
}

void config_reset() {
    prefs.begin("stravamon", false);
    prefs.clear();
    prefs.end();
}

void config_save(const Config &cfg) {
    prefs.begin("stravamon", false); // read-write namespace
    prefs.putString("wifi_ssid",   cfg.wifi_ssid);
    prefs.putString("wifi_pass",   cfg.wifi_password);
    prefs.putString("srv_url",     cfg.strava_server_url);
    prefs.putUChar( "brightness",  cfg.brightness);
    prefs.putUShort("switch_sec",  cfg.screen_switch_secs);
    prefs.putUShort("refresh_min", cfg.refresh_interval_min);
    prefs.putUChar( "hist_months", cfg.history_months);
    prefs.putUChar( "anim_pct",    cfg.anim_top_pct);
    prefs.putUShort("anim_ms",     cfg.anim_period_ms);
    prefs.putUChar( "flip_scr",    cfg.flip_screen);
    prefs.putString("mqtt_host",   cfg.mqtt_broker);
    prefs.putUShort("mqtt_port",   cfg.mqtt_port);
    prefs.putString("mqtt_lcd",    cfg.mqtt_lcd_topic);
    prefs.remove("rgb_bright");
    prefs.remove("rgb_pmin");
    prefs.remove("rgb_pmax");
    prefs.remove("rgb_smax");
    prefs.remove("mqtt_ctopic");
    prefs.remove("mqtt_ltopic");
    prefs.end();
}
