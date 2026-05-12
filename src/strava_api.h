#pragma once
#include <Arduino.h>

#define GRID_WEEKS 53
#define GRID_DAYS  7

// ── Per-day exercise data ─────────────────────────────────────────────────────

struct ExerciseDay {
    float   load;            // moving time in minutes for this day
    uint8_t load_level;      // 0 = none | 1 = low (1-30) | 2 = medium (31-60)
                            // 3 = high (61-120) | 4 = very high (>120)
    float   calories;        // calories burned for this day
    uint8_t calories_level;  // 0 = none | 1 = low (1-500) | 2 = medium (501-750)
                            // 3 = high (751-1000) | 4 = very high (>1000)
};

// ── Full dataset (53 weeks x 7 days) ─────────────────────────────────────────
// Layout is days[week][weekday], week 52 = current week, weekday 0 = Sunday.

struct StravaData {
    ExerciseDay days[GRID_WEEKS][GRID_DAYS];

    uint8_t  week_count;              // weeks populated after last fetch

    float    total_load;              // total minutes across the full grid
    uint32_t total_activities;        // number of days with load > 0

    float    busiest_day_load;        // peak daily load in minutes
    uint8_t  busiest_day_day;         // day-of-month 1-31 of busiest day
    uint8_t  busiest_day_month;       // month 1-12 of busiest day

    uint16_t current_month_load;      // total minutes (rounded) in current month
    uint8_t  current_month;           // 1-12

    int32_t  anchor_week_start_days;  // days since Unix epoch for week-52 Sunday
    int32_t  current_day_days;        // days since epoch for the bridge generation date
    int32_t  latest_data_day_days;    // days since epoch for the most recent day

    bool     valid;                   // false until first successful fetch
};

// Convert a load value (minutes) to a display level 0-4
uint8_t exercise_load_level(float load);

// Convert a calorie value to a display level 0-4
uint8_t calorie_burn_level(float calories);

// Fetch exercise load from the strava_bridge server.
//   url  – full URL, e.g. "http://192.168.1.54:8082/api/exercise-load"
// Returns true on success; populates data in-place.
bool strava_fetch(const char *url, StravaData &data);
