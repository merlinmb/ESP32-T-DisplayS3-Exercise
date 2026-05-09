#include "rgb_led.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <math.h>

// Use the pin from the board header if NEOPIXEL_SUPPORTED is defined.
// The T-Display S3 has no built-in NeoPixel; define NEOPIXEL_SUPPORTED in the
// pins header to enable an external one.
#ifdef NEOPIXEL_SUPPORTED
  #ifndef NEOPIXEL_PIN
    #define NEOPIXEL_PIN 21
  #endif
  static Adafruit_NeoPixel s_pixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
  static bool s_pixel_enabled = true;
#else
  // Stub pixel object (not used at runtime)
  static Adafruit_NeoPixel s_pixel(1, 21, NEO_GRB + NEO_KHZ800);
  static bool s_pixel_enabled = false;
#endif

// Current breathing parameters (updated by rgb_led_update_params)
static uint32_t s_period_ms   = 8000;
static uint8_t  s_base_r      = 20;
static uint8_t  s_base_g      = 20;
static uint8_t  s_base_b      = 40;

// Previous written color (to avoid redundant writes)
static uint8_t s_prev_r = 255;
static uint8_t s_prev_g = 255;
static uint8_t s_prev_b = 255;

// Current brightness scalar 0-100 (applied as multiplier in tick)
static uint8_t s_brightness_pct = 100;

void rgb_led_init(const Config &cfg) {
    s_brightness_pct = cfg.rgb_brightness;
    if (!s_pixel_enabled) return;
    s_pixel.begin();
    s_pixel.setBrightness(255);
    s_pixel.clear();
    s_pixel.show();
}

void rgb_led_update_params(const StravaData &data, const Config &cfg) {
    if (!data.valid) {
        // No data yet: slow white-blue idle
        s_period_ms = cfg.rgb_period_max_ms;
        s_base_r    = 20;
        s_base_g    = 20;
        s_base_b    = 40;
        return;
    }

    // Use today's exercise load as the primary signal.
    // anchor_week_start_days is the Sunday of week 52; today is offset 0-6 from it.
    uint8_t today_weekday = 0;
    if (data.anchor_week_start_days > 0 && data.latest_data_day_days > 0) {
        int32_t diff = data.latest_data_day_days - data.anchor_week_start_days;
        if (diff >= 0 && diff < 7) today_weekday = (uint8_t)diff;
    }
    float today_load = data.days[52][today_weekday].load;

    // rgb_streak_max is now reused as "load (minutes) that achieves max LED intensity"
    float ratio = today_load / (float)cfg.rgb_streak_max;
    if (ratio > 1.0f) ratio = 1.0f;

    if (today_load <= 0.0f) {
        // No activity today: slow dim idle (blue-white)
        s_period_ms = cfg.rgb_period_max_ms;
        s_base_r    = 20;
        s_base_g    = 20;
        s_base_b    = 40;
        return;
    }

    // Breath speed: more load = faster
    s_period_ms = (uint32_t)(cfg.rgb_period_max_ms -
                  ratio * (cfg.rgb_period_max_ms - cfg.rgb_period_min_ms));

    // Color: green (low) -> amber -> red (high), matching level palette
    uint8_t lvl = data.days[52][today_weekday].level;
    switch (lvl) {
        case 1: s_base_r = 39;  s_base_g = 174; s_base_b = 96;  break; // green
        case 2: s_base_r = 243; s_base_g = 156; s_base_b = 18;  break; // amber
        case 3: s_base_r = 230; s_base_g = 126; s_base_b = 34;  break; // orange
        default:s_base_r = 231; s_base_g = 76;  s_base_b = 60;  break; // red
    }
}

void rgb_led_tick() {
    if (!s_pixel_enabled) return;

    uint32_t t = millis();
    float phase      = (float)(t % s_period_ms) / (float)s_period_ms;
    float brightness = (sinf(phase * 2.0f * M_PI - M_PI / 2.0f) + 1.0f) / 2.0f;
    float led_scale  = (float)s_brightness_pct / 100.0f;

    uint8_t r = (uint8_t)(s_base_r * brightness * led_scale);
    uint8_t g = (uint8_t)(s_base_g * brightness * led_scale);
    uint8_t b = (uint8_t)(s_base_b * brightness * led_scale);

    if (abs((int)r - (int)s_prev_r) < 1 &&
        abs((int)g - (int)s_prev_g) < 1 &&
        abs((int)b - (int)s_prev_b) < 1) return;

    s_prev_r = r; s_prev_g = g; s_prev_b = b;
    s_pixel.setPixelColor(0, s_pixel.Color(r, g, b));
    s_pixel.show();
}

void rgb_led_set_brightness_pct(uint8_t pct) {
    if (pct > 100) pct = 100;
    s_brightness_pct = pct;
    s_prev_r = 255; s_prev_g = 255; s_prev_b = 255; // force redraw
}
