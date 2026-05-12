#include "display_grid.h"
#include <Arduino.h>
#include <string.h>

// ── Layout constants (landscape 320×170) ─────────────────────────────────────
#define SCREEN_W     320
#define SCREEN_H     170
#define CELL_H       12
#define MIN_COL_GAP  1
#define ROW_GAP      2
#define FOOTER_H     28
#define SIDE_PAD     3
#define DOW_W        8
#define DOW_GAP      3
#define LEFT_LABEL_W (DOW_W + DOW_GAP)
#define LABEL_FONT_H 16        // Font2 height
#define LABEL_GAP    4
#define GRID_PIXEL_H ((GRID_DAYS * CELL_H) + ((GRID_DAYS - 1) * ROW_GAP))
#define GRID_UNIT_H  (LABEL_FONT_H + LABEL_GAP + GRID_PIXEL_H)
#define UNIT_TOP     ((SCREEN_H - FOOTER_H - GRID_UNIT_H) / 2)
#define GRID_TOP     (UNIT_TOP + LABEL_FONT_H + LABEL_GAP)
#define MONTH_GAP    3
#define MONTH_SEP_OVERHANG 6
#define CELL_RADIUS  1

// ── Calendar helpers ──────────────────────────────────────────────────────────
static void civil_from_days(int32_t z, int &yr, int &mo, int &dy) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yr = (int)(yoe + era * 400);
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp  = (5 * doy + 2) / 153;
    dy = (int)(doy - (153 * mp + 2) / 5 + 1);
    mo = (int)(mp + (mp < 10 ? 3 : -9));
    yr += (mo <= 2);
}

static int32_t days_from_civil(int yr, unsigned mo, unsigned dy) {
    yr -= (int)(mo <= 2);
    const int era = (yr >= 0 ? yr : yr - 399) / 400;
    const unsigned yoe = (unsigned)(yr - era * 400);
    const unsigned doy = (153u * (mo + (mo > 2u ? (unsigned)-3 : 9u)) + 2u) / 5u + dy - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int)doe - 719468;
}

static uint8_t weekday_sun0(int32_t d) {
    int w = (int)((d + 4) % 7);
    if (w < 0) w += 7;
    return (uint8_t)w;
}

// ── Grid layout ───────────────────────────────────────────────────────────────
struct GridLayout {
    int visible_columns;
    int grid_left;
    int cell_w;
    int col_gap;
};

struct MonthMarker { int column; int month; };

struct VisibleMonth {
    int year;
    int month;
    int start_column;
    int column_count;
    int32_t start_day;
    int32_t end_day;
};

static int month_extra_gap(int col_gap);
static int month_x_offset(int month_index, int col_gap);

static uint8_t clamp_history_months(uint8_t m) {
    if (m < kMinHistoryMonths) return kDefaultHistoryMonths;
    if (m > kMaxHistoryMonths) return kMaxHistoryMonths;
    return m;
}

static int approximate_visible_weeks(uint8_t history_months) {
    int weeks = (int)((history_months * 31u + 6u) / 7u);
    if (weeks < 1) weeks = 1;
    if (weeks > GRID_WEEKS) weeks = GRID_WEEKS;
    return weeks;
}

static void shift_months(int &yr, int &mo, int delta) {
    mo += delta;
    while (mo < 1)  { mo += 12; yr--; }
    while (mo > 12) { mo -= 12; yr++; }
}

static int32_t reference_calendar_day(const StravaData &data) {
    if (data.current_day_days != 0) return data.current_day_days;
    if (data.latest_data_day_days != 0) return data.latest_data_day_days;
    if (data.anchor_week_start_days != 0) return data.anchor_week_start_days + 6;
    return 0;
}

static int build_visible_months(const StravaData &data, uint8_t history_months,
                                VisibleMonth *out, int max_out, int &total_columns) {
    total_columns = 0;
    if (!out || max_out <= 0) return 0;

    int32_t ref = reference_calendar_day(data);
    if (ref == 0) return 0;

    int yr = 0, mo = 0, dy = 0;
    civil_from_days(ref, yr, mo, dy);
    shift_months(yr, mo, 1 - (int)history_months);

    int count = 0;
    for (uint8_t i = 0; i < history_months && count < max_out; i++) {
        int32_t month_start = days_from_civil(yr, (unsigned)mo, 1);
        int next_yr = yr;
        int next_mo = mo;
        shift_months(next_yr, next_mo, 1);
        int32_t month_end = days_from_civil(next_yr, (unsigned)next_mo, 1);
        int32_t visible_end = month_end;
        if (ref >= month_start && ref < month_end) visible_end = ref + 1;

        int first_row = (int)weekday_sun0(month_start);
        int day_count = (int)(visible_end - month_start);
        int column_count = (first_row + day_count + GRID_DAYS - 1) / GRID_DAYS;

        out[count].year         = yr;
        out[count].month        = mo;
        out[count].start_column = total_columns;
        out[count].column_count = column_count;
        out[count].start_day    = month_start;
        out[count].end_day      = visible_end;
        total_columns += column_count;
        count++;

        yr = next_yr;
        mo = next_mo;
    }

    return count;
}

static GridLayout build_grid_layout(int visible_columns, int month_count) {
    GridLayout lo{};
    lo.visible_columns = (visible_columns > 0) ? visible_columns : 1;

    int avail_w = SCREEN_W - (SIDE_PAD * 2) - LEFT_LABEL_W;
    int month_gap_total = ((month_count > 1) ? (month_count - 1) : 0)
                       * month_extra_gap(MIN_COL_GAP);
    int content_avail_w = avail_w - month_gap_total;
    lo.col_gap = 1;
    lo.cell_w = 1;

    for (int gap = 1; gap >= MIN_COL_GAP; gap--) {
        int gap_total = (lo.visible_columns - 1) * gap;
        int cell_w = (content_avail_w - gap_total) / lo.visible_columns;
        if (cell_w >= 1) {
            lo.col_gap = gap;
            lo.cell_w = cell_w;
            break;
        }
    }

    month_gap_total = ((month_count > 1) ? (month_count - 1) : 0)
                   * month_extra_gap(lo.col_gap);
    int gap_total    = (lo.visible_columns - 1) * lo.col_gap;
    int grid_px_w    = lo.visible_columns * lo.cell_w + gap_total + month_gap_total;
    lo.grid_left     = SIDE_PAD + LEFT_LABEL_W + ((avail_w - grid_px_w) / 2);
    return lo;
}

static const char *month_abbr(int mo) {
    static const char *M[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    return (mo >= 1 && mo <= 12) ? M[mo - 1] : "";
}

static int collect_month_markers(const VisibleMonth *months, int month_count,
                                 MonthMarker *out, int max_out) {
    if (!months || !out || max_out <= 0) return 0;

    int count = 0;
    for (int i = 0; i < month_count && count < max_out; i++) {
        out[count].column = months[i].start_column;
        out[count].month  = months[i].month;
        count++;
    }
    return count;
}

static int month_extra_gap(int col_gap) {
    int extra = MONTH_GAP - col_gap;
    return (extra > 0) ? extra : 0;
}

static int month_x_offset(int month_index, int col_gap) {
    return month_index * month_extra_gap(col_gap);
}

static const char *weekday_letter_for_row(int row) {
    static const char *L[7] = { "S","M","T","W","T","F","S" };  // row 0 = Sunday
    return (row >= 0 && row < 7) ? L[row] : "";
}

static void fill_grid_cell(TFT_eSprite &spr, int x, int y, int w, int h,
                           uint16_t color) {
    if (w <= 2 || h <= 2) {
        spr.fillRect(x, y, w, h, color);
        return;
    }
    spr.fillRoundRect(x, y, w, h, CELL_RADIUS, color);
}

static void outline_grid_cell(TFT_eSprite &spr, int x, int y, int w, int h,
                              uint16_t color) {
    if (w <= 2 || h <= 2) {
        spr.drawRect(x, y, w, h, color);
        return;
    }
    spr.drawRoundRect(x, y, w, h, CELL_RADIUS, color);
}

static bool today_cell_blink_on(float anim_phase) {
    return anim_phase >= 0.5f;
}

// ── Animation threshold ───────────────────────────────────────────────────────
// Returns the minimum level that should be animated (0 = none animated).
static uint8_t day_level_for_epoch(const StravaData &data, int32_t day_epoch,
                                   bool is_calories) {
    if (!data.valid || data.anchor_week_start_days == 0) return 0;

    uint8_t wd    = weekday_sun0(day_epoch);
    int32_t ws    = day_epoch - (int32_t)wd;
    int32_t delta = data.anchor_week_start_days - ws;
    if (delta < 0 || (delta % 7) != 0) return 0;

    int week_diff = (int)(delta / 7);
    if (week_diff >= GRID_WEEKS) return 0;

    int week = (GRID_WEEKS - 1) - week_diff;
    uint8_t lvl = is_calories ? data.days[week][wd].calories_level
                              : data.days[week][wd].load_level;
    return (lvl > 4) ? 4 : lvl;
}

static uint8_t animation_level_threshold(const StravaData &data,
                                         const VisibleMonth *months, int month_count,
                                         bool is_cal, uint8_t top_pct,
                                         int32_t today_day) {
    if (top_pct == 0 || !data.valid || !months || month_count <= 0) return 5;

    int level_cnt[5] = {};

    for (int mi = 0; mi < month_count; mi++) {
        int32_t last_day = months[mi].end_day;
        if (today_day > 0 && last_day > today_day + 1) last_day = today_day + 1;
        for (int32_t day = months[mi].start_day; day < last_day; day++) {
            uint8_t lvl = day_level_for_epoch(data, day, is_cal);
            level_cnt[lvl]++;
        }
    }

    int active      = level_cnt[1] + level_cnt[2] + level_cnt[3] + level_cnt[4];
    int anim_target = (active * (int)top_pct + 99) / 100;
    int counted     = 0;
    for (int l = 4; l >= 1; l--) {
        counted += level_cnt[l];
        if (counted >= anim_target) return (uint8_t)l;
    }
    return 5;  // nothing meets threshold
}

// ── Public render function ────────────────────────────────────────────────────
void display_grid_render(TFT_eSprite &spr, const StravaData &data,
                         bool is_calories, float anim_phase, const Config &cfg) {
    const uint8_t history_mo = clamp_history_months(cfg.history_months);
    VisibleMonth months[kMaxHistoryMonths];
    int total_columns = approximate_visible_weeks(history_mo);
    int month_count   = build_visible_months(data, history_mo, months,
                                             kMaxHistoryMonths, total_columns);
    const GridLayout lo      = build_grid_layout(total_columns, month_count);
    const int32_t today_day  = data.valid ? reference_calendar_day(data) : 0;
    const uint8_t anim_min   = data.valid
        ? animation_level_threshold(data, months, month_count, is_calories,
                                    cfg.anim_top_pct ? cfg.anim_top_pct : 20,
                                    today_day)
        : 5u;

    // ── Day-of-week labels (left column) ─────────────────────────────────────
    spr.setTextFont(2);
    spr.setTextColor(RGB565(0x8F, 0x99, 0xA3));
    spr.setTextDatum(TL_DATUM);
    for (int row = 0; row < GRID_DAYS; row++) {
        int y = GRID_TOP + row * (CELL_H + ROW_GAP)
              + (CELL_H - LABEL_FONT_H) / 2;
        spr.drawString(weekday_letter_for_row(row), SIDE_PAD, y);
    }

    // ── Grid cells ────────────────────────────────────────────────────────────
    if (!data.valid || month_count <= 0) {
        for (int col = 0; col < lo.visible_columns; col++) {
            int cell_x = lo.grid_left + col * (lo.cell_w + lo.col_gap);
            for (int row = 0; row < GRID_DAYS; row++) {
                int cell_y = GRID_TOP + row * (CELL_H + ROW_GAP);
                fill_grid_cell(spr, cell_x, cell_y, lo.cell_w, CELL_H, LEVEL_BASE[0]);
            }
        }
    } else {
        for (int mi = 0; mi < month_count; mi++) {
            int first_row = (int)weekday_sun0(months[mi].start_day);
            for (int32_t day = months[mi].start_day; day < months[mi].end_day; day++) {
                int day_offset = (int)(day - months[mi].start_day);
                int slot       = first_row + day_offset;
                int col        = months[mi].start_column + (slot / GRID_DAYS);
                int row        = slot % GRID_DAYS;

                if (today_day > 0 && day > today_day) continue;

                int cell_x = lo.grid_left + col * (lo.cell_w + lo.col_gap)
                           + month_x_offset(mi, lo.col_gap);
                int cell_y = GRID_TOP + row * (CELL_H + ROW_GAP);
                uint8_t lvl = day_level_for_epoch(data, day, is_calories);
                bool is_today = (today_day > 0 && day == today_day);
                bool animate_level = (lvl > 0 && lvl >= anim_min);
                bool today_blink_on = is_today && today_cell_blink_on(anim_phase);
                uint16_t base_color = LEVEL_BASE[lvl];
                uint16_t bright_color = animate_level
                    ? lerp_color(LEVEL_BRIGHT[lvl], C_WHITE, 0.18f)
                    : base_color;
                uint16_t today_fill_color = lerp_color(base_color, C_WHITE, 0.30f);
                uint16_t color = animate_level
                    ? lerp_color(base_color, bright_color, anim_phase)
                    : (today_blink_on ? today_fill_color : base_color);
                fill_grid_cell(spr, cell_x, cell_y, lo.cell_w, CELL_H, color);
                if (animate_level || is_today) {
                    uint16_t border_color = is_today
                        ? C_WHITE
                        : lerp_color(base_color,
                                     lerp_color(bright_color, C_WHITE, 0.45f),
                                     anim_phase);
                    outline_grid_cell(spr, cell_x, cell_y, lo.cell_w, CELL_H, border_color);
                }
            }
        }
    }

    // ── Month labels (top) ────────────────────────────────────────────────────
    MonthMarker markers[kMaxHistoryMonths];
    int marker_count = collect_month_markers(months, month_count, markers,
                                             kMaxHistoryMonths);
    // Draw month labels left-aligned at the start of each month's first column,
    // so they align visually with the separator line on the left.
    if (marker_count > 0) {
        spr.setTextFont(2);
        spr.setTextColor(TFT_WHITE);
        spr.setTextDatum(TL_DATUM);
        for (int mi = 0; mi < marker_count; mi++) {
            int mx = lo.grid_left + markers[mi].column * (lo.cell_w + lo.col_gap)
                   + month_x_offset(mi, lo.col_gap);
            spr.drawString(month_abbr(markers[mi].month), mx, UNIT_TOP);
        }
    }

        // ── Footer bar ────────────────────────────────────────────────────────────
    const int fy = SCREEN_H - FOOTER_H;
    spr.fillRect(0, fy, SCREEN_W, FOOTER_H, TFT_BLACK);
    
    // ── Month separators (1 px grey vertical lines, 1px pad each side) ────────
    spr.setTextDatum(TL_DATUM);
    for (int mi = 1; mi < marker_count; mi++) {
        int month_start_x = lo.grid_left + markers[mi].column * (lo.cell_w + lo.col_gap)
                          + month_x_offset(mi, lo.col_gap);
        int x = month_start_x - ((MONTH_GAP + 1) / 2);
        spr.drawFastVLine(x,
            GRID_TOP - MONTH_SEP_OVERHANG,
            GRID_PIXEL_H + MONTH_SEP_OVERHANG * 2,
            RGB565(0x50, 0x50, 0x50));
    }



    // Title (horizontally centred, Strava orange)
    spr.setTextFont(2);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(C_STRAVA);
    const char *title = is_calories ? "Calories Burned" : "Exercise Load";
    spr.drawString(title, SCREEN_W / 2, fy + FOOTER_H / 2);

    // Legend squares (right: 5 coloured 5×12 px squares + "More" label)
    spr.setTextColor(TFT_WHITE);
    spr.setTextDatum(MR_DATUM);
    spr.drawString("More", SCREEN_W - SIDE_PAD, fy + FOOTER_H / 2);
    // "More" at Font2 is ~4 chars * ~8px = ~32px wide; right edge ~314, left ~282
    int sq_right = SCREEN_W - SIDE_PAD - 39;  // shifted 5px left so squares don't overlap "More"
    for (int i = 4; i >= 0; i--) {
        int sq_x = sq_right - (4 - i) * 7;
        spr.fillRect(sq_x, fy + (FOOTER_H - CELL_H) / 2, 5, CELL_H, LEVEL_BASE[i]);
    }
}

