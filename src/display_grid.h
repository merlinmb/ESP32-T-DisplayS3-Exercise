#pragma once
#include <lvgl.h>
#include "strava_api.h"
#include "config.h"

enum GridMetric {
	GRID_METRIC_LOAD = 0,
	GRID_METRIC_CALORIES,
	GRID_METRIC_COUNT,
};

// Build the grid screen. Call once after LVGL init.
lv_obj_t *display_grid_build(GridMetric metric, const char *title, const Config &cfg);

// Update cell colours from fresh data (does NOT start animations).
void display_grid_update(GridMetric metric, const StravaData &data, const Config &cfg);

// Start breathing animations for the given metric's grid screen.
// Call only after the screen is active to avoid LVGL dirty-area issues on inactive screens.
void display_grid_start_animations(GridMetric metric, const StravaData &data, const Config &cfg);

// Stop all breathing animations (call before data refresh or when leaving the screen).
void display_grid_stop_animations(GridMetric metric);
