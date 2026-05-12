#pragma once
#include "display.h"
#include "strava_api.h"

// Render the 7-day calorie sparkline screen into the provided sprite.
void display_calorie_trend_render(TFT_eSprite &spr, const StravaData &data);