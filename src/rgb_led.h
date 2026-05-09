#pragma once
#include "config.h"
#include "strava_api.h"

void rgb_led_init(const Config &cfg);
void rgb_led_update_params(const StravaData &data, const Config &cfg);
void rgb_led_tick();

// Set RGB LED brightness by percentage (0 = off, 100 = full).
void rgb_led_set_brightness_pct(uint8_t pct);
