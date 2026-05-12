#pragma once
#include "display.h"
#include "strava_api.h"
#include "config.h"

enum GridMetric {
    GRID_METRIC_LOAD = 0,
    GRID_METRIC_CALORIES,
    GRID_METRIC_COUNT,
};

// Render the heatmap grid screen directly into the provided sprite.
// anim_phase 0.0-1.0 drives high-activity breathing and the current-day blink.
void display_grid_render(TFT_eSprite &spr, const StravaData &data,
                         bool is_calories, float anim_phase, const Config &cfg);
