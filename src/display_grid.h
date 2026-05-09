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

// Update cell colours and reassign animations from fresh data.
void display_grid_update(GridMetric metric, const StravaData &data, const Config &cfg);

// Stop all breathing animations (call before data refresh).
void display_grid_stop_animations(GridMetric metric);
