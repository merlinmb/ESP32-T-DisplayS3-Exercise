#pragma once
#include <lvgl.h>
#include "strava_api.h"

// Build the stats screen. Call once after LVGL init.
lv_obj_t *display_stats_build(const char *title);

// Update labels from fresh data.
void display_stats_update(const StravaData &data);

// Update the "last updated Xm ago" footer. Call with minutes since last fetch.
void display_stats_set_age(uint32_t minutes_ago);
