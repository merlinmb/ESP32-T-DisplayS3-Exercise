#include "mqtt_client.h"
#include "screen_brightness.h"
#include "config.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>

static WiFiClient   s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);
static Config      *s_cfg          = nullptr;
static bool         s_enabled      = false;
static bool         s_save_pending = false; // set by callback, acted on in tick()

static constexpr char kMqttClientId[] = "StravaMonitor";
static constexpr char kMqttDeviceName[] = "T-DisplayS3-StravaMonitor";

static const uint32_t RECONNECT_INTERVAL_MS = 5000;
static uint32_t s_last_reconnect_ms = 0;

static bool mqtt_has_topic(const char *topic) {
    return topic && topic[0] != '\0';
}

static bool mqtt_publish_stat(const char *suffix, const String &payload) {
    char topic[128];
    snprintf(topic, sizeof(topic), "stat/mcmddevices/%s", suffix);
    bool ok = s_mqtt.publish(topic, payload.c_str());
    if (!ok) {
        Serial.printf("[MQTT] Publish failed for topic '%s'\n", topic);
    }
    return ok;
}

static void mqttTransmitInitStat(String deviceName) {
    String payload;
    payload.reserve(128);
    payload = "{\"value1\":\"" + WiFi.localIP().toString() +
              "\",\"value2\":\"" + WiFi.macAddress() +
              "\",\"value3\":\"" + deviceName + "\"}";
    mqtt_publish_stat("init", payload);
}

static void mqttTransmitInitStat() {
    mqttTransmitInitStat(String(kMqttDeviceName));
}

static bool parse_brightness_payload(byte *payload, unsigned int len, uint8_t &pct_out) {
    if (!payload || len == 0 || len >= 16) return false;

    char buf[16];
    memcpy(buf, payload, len);
    buf[len] = '\0';

    char *start = buf;
    while (*start && isspace((unsigned char)*start)) ++start;

    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    *end = '\0';

    if (*start == '"' && end - start >= 2 && end[-1] == '"') {
        ++start;
        --end;
        *end = '\0';
        while (*start && isspace((unsigned char)*start)) ++start;
        while (end > start && isspace((unsigned char)end[-1])) --end;
        *end = '\0';
    }

    if (*start == '\0') return false;

    char *parse_end = nullptr;
    long raw = strtol(start, &parse_end, 10);
    if (parse_end == start) return false;

    while (*parse_end && isspace((unsigned char)*parse_end)) ++parse_end;
    if (*parse_end == '.') {
        ++parse_end;
        while (*parse_end == '0') ++parse_end;
        while (*parse_end && isspace((unsigned char)*parse_end)) ++parse_end;
    }
    if (*parse_end != '\0') return false;

    if (raw < 0) raw = 0;
    if (raw > 100) raw = 100;
    pct_out = (uint8_t)raw;
    return true;
}

// ── Message handler ───────────────────────────────────────────────────────────

static void on_message(const char *topic, byte *payload, unsigned int len) {
    if (!s_cfg || !topic || !payload) return;
    if (!mqtt_has_topic(s_cfg->mqtt_lcd_topic) || strcmp(topic, s_cfg->mqtt_lcd_topic) != 0) return;

    uint8_t pct = 0;
    if (!parse_brightness_payload(payload, len, pct)) {
        unsigned int preview_len = len < 32 ? len : 32;
        Serial.printf("[MQTT] Ignored brightness payload on '%s': '%.*s' (len=%u)\n",
                      topic,
                      (int)preview_len,
                      (const char *)payload,
                      len);
        return;
    }

    Serial.printf("[MQTT] LCD brightness -> %u%%\n", pct);
    s_cfg->brightness = pct;
    set_screen_brightness_pct(pct);
    s_save_pending = true;
}

// ── Connection helper ─────────────────────────────────────────────────────────

static bool try_connect() {
    if (!s_cfg) return false;
    Serial.printf("[MQTT] Connecting to %s:%u ...\n",
                  s_cfg->mqtt_broker, s_cfg->mqtt_port);
    // Cap the blocking TCP connect to 3 s so loop() is not starved.
    s_wifi_client.setTimeout(3000);
    if (!s_mqtt.connect(kMqttClientId)) {
        Serial.printf("[MQTT] Failed, state=%d\n", s_mqtt.state());
        return false;
    }
    if (mqtt_has_topic(s_cfg->mqtt_lcd_topic)) {
        bool subscribed = s_mqtt.subscribe(s_cfg->mqtt_lcd_topic);
        Serial.printf("[MQTT] Connected. %s '%s'\n",
                      subscribed ? "Subscribed to" : "Subscribe failed for",
                      s_cfg->mqtt_lcd_topic);
    } else {
        Serial.println("[MQTT] Connected. No topic configured");
    }
    mqttTransmitInitStat();
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────

void mqtt_client_init(Config &cfg) {
    s_cfg = &cfg;
    if (cfg.mqtt_broker[0] == '\0') {
        Serial.println("[MQTT] No broker configured — disabled");
        return;
    }
    s_enabled = true;
    s_mqtt.setServer(cfg.mqtt_broker, cfg.mqtt_port);
    s_mqtt.setCallback(on_message);
    try_connect();
}

void mqtt_client_reinit() {
    if (!s_cfg) return;
    // Disconnect cleanly from old broker
    if (s_mqtt.connected()) s_mqtt.disconnect();
    s_enabled = false;
    if (s_cfg->mqtt_broker[0] == '\0') {
        Serial.println("[MQTT] Broker cleared — disabled");
        return;
    }
    s_enabled = true;
    s_mqtt.setServer(s_cfg->mqtt_broker, s_cfg->mqtt_port);
    s_last_reconnect_ms = 0;
    try_connect();
}

void mqtt_client_tick() {
    if (!s_enabled || !s_cfg) return;

    // Flush any config save requested by the message callback.
    if (s_save_pending) {
        s_save_pending = false;
        config_save(*s_cfg);
    }

    if (WiFi.status() != WL_CONNECTED) return;
    if (s_mqtt.connected()) {
        s_mqtt.loop();
        return;
    }
    uint32_t now = millis();
    if (now - s_last_reconnect_ms >= RECONNECT_INTERVAL_MS) {
        s_last_reconnect_ms = now;
        try_connect();
    }
}
