#pragma once

#include <lvgl.h>
#include "strava_api.h"

lv_obj_t *display_calorie_trend_build(const char *title);
void display_calorie_trend_update(const StravaData &data);