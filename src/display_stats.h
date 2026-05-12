#pragma once
#include "display.h"
#include "strava_api.h"

// Render the stats summary screen into the provided sprite.
// age_minutes: minutes since the last successful data fetch.
void display_stats_render(TFT_eSprite &spr, const StravaData &data,
                          uint32_t age_minutes);
