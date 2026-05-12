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
#define MONTH_SEP_OVERHANG 4

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
    int visible_weeks;
    int grid_left;
    int cell_w;
    int col_gap;
};

struct MonthMarker { int week; int month; };

static uint8_t clamp_history_months(uint8_t m) {
    if (m < kMinHistoryMonths) return kDefaultHistoryMonths;
    if (m > kMaxHistoryMonths) return kMaxHistoryMonths;
    return m;
}

static int data_week_count(const StravaData &data) {
    int wc = (int)data.week_count;
    return (wc > GRID_WEEKS) ? GRID_WEEKS : wc;
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

static int visible_week_count_for_history(const StravaData &data, uint8_t history_months) {
    const int fallback = approximate_visible_weeks(history_months);
    if (data.anchor_week_start_days == 0) return fallback;

    int yr = 0, mo = 0, dy = 0;
    int32_t ref = (data.latest_data_day_days != 0)
                  ? data.latest_data_day_days
                  : (data.anchor_week_start_days + 6);
    civil_from_days(ref, yr, mo, dy);
    shift_months(yr, mo, 1 - (int)history_months);
    int32_t start_of_month = days_from_civil(yr, (unsigned)mo, 1);
    int32_t first_sunday   = start_of_month - weekday_sun0(start_of_month);
    int32_t delta_days     = data.anchor_week_start_days - first_sunday;
    if (delta_days < 0) return fallback;

    int weeks = (int)(delta_days / 7) + 1;
    int avail = data_week_count(data);
    if (avail > 0 && weeks > avail) weeks = avail;
    if (weeks < 1) weeks = 1;
    if (weeks > GRID_WEEKS) weeks = GRID_WEEKS;
    return weeks;
}

static GridLayout build_grid_layout(const StravaData &data, const Config &cfg) {
    GridLayout lo{};
    lo.visible_weeks = visible_week_count_for_history(data, clamp_history_months(cfg.history_months));
    lo.col_gap       = 3;   // 1px separator + 1px pad each side at month boundaries
    int avail_w      = SCREEN_W - (SIDE_PAD * 2) - LEFT_LABEL_W;
    int gap_total    = (lo.visible_weeks - 1) * lo.col_gap;
    lo.cell_w        = (avail_w - gap_total) / lo.visible_weeks;
    if (lo.cell_w < 1) lo.cell_w = 1;
    int grid_px_w    = lo.visible_weeks * lo.cell_w + gap_total;
    lo.grid_left     = SIDE_PAD + LEFT_LABEL_W + ((avail_w - grid_px_w) / 2);
    return lo;
}

static int first_visible_week_index(int total_weeks, int visible_weeks) {
    if (visible_weeks >= total_weeks) return 0;
    return total_weeks - visible_weeks;
}

static const char *month_abbr(int mo) {
    static const char *M[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    return (mo >= 1 && mo <= 12) ? M[mo - 1] : "";
}

static int collect_month_markers(const StravaData &data, const GridLayout &lo,
                                 MonthMarker *out, int max_out) {
    if (data.anchor_week_start_days == 0 || !out || max_out <= 0) return 0;
    int total_weeks = data_week_count(data);
    if (total_weeks <= 0) total_weeks = GRID_WEEKS;
    int start_week  = first_visible_week_index(total_weeks, lo.visible_weeks);
    int count = 0;
    int prev_mo = -1;
    for (int vw = 0; vw < lo.visible_weeks && count < max_out; vw++) {
        int dw = start_week + vw;
        int32_t week_sun = data.anchor_week_start_days - (int32_t)(total_weeks - 1 - dw) * 7;
        int yr = 0, mo = 0, dy = 0;
        civil_from_days(week_sun + 3, yr, mo, dy);
        if (mo != prev_mo) {
            out[count].week  = vw;
            out[count].month = mo;
            count++;
            prev_mo = mo;
        }
    }
    return count;
}

static const char *weekday_letter_for_row(int row) {
    static const char *L[7] = { "M","T","W","T","F","S","S" };
    return (row >= 0 && row < 7) ? L[row] : "";
}

// ── Animation threshold ───────────────────────────────────────────────────────
// Returns the minimum level that should be animated (0 = none animated).
static uint8_t animation_level_threshold(const StravaData &data, const GridLayout &lo,
                                         bool is_cal, uint8_t top_pct) {
    if (top_pct == 0) return 5;  // above max → nothing animates

    int total_weeks  = data_week_count(data);
    if (total_weeks <= 0) total_weeks = GRID_WEEKS;
    int start_week   = first_visible_week_index(total_weeks, lo.visible_weeks);

    int level_cnt[5] = {};
    for (int w = 0; w < lo.visible_weeks; w++) {
        int dw = start_week + w;
        if (dw >= GRID_WEEKS) break;
        for (int d = 0; d < GRID_DAYS; d++) {
            uint8_t lvl = is_cal ? data.days[dw][d].calories_level
                                 : data.days[dw][d].load_level;
            if (lvl > 4) lvl = 4;
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
    const GridLayout lo      = build_grid_layout(data, cfg);
    const uint8_t anim_min   = data.valid
        ? animation_level_threshold(data, lo, is_calories,
                                    cfg.anim_top_pct ? cfg.anim_top_pct : 20)
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
    int total_weeks = data.valid ? data_week_count(data) : 0;
    if (total_weeks <= 0) total_weeks = GRID_WEEKS;
    int start_week = first_visible_week_index(total_weeks, lo.visible_weeks);

    for (int w = 0; w < lo.visible_weeks; w++) {
        int dw     = start_week + w;
        if (dw >= GRID_WEEKS) break;
        int cell_x = lo.grid_left + w * (lo.cell_w + lo.col_gap);
        for (int d = 0; d < GRID_DAYS; d++) {
            uint8_t lvl = 0;
            if (data.valid) {
                lvl = is_calories ? data.days[dw][d].calories_level
                                  : data.days[dw][d].load_level;
                if (lvl > 4) lvl = 4;
            }
            int cell_y = GRID_TOP + d * (CELL_H + ROW_GAP);
            uint16_t color = (lvl > 0 && lvl >= anim_min)
                ? lerp_color(LEVEL_BASE[lvl], LEVEL_BRIGHT[lvl], anim_phase)
                : LEVEL_BASE[lvl];
            spr.fillRect(cell_x, cell_y, lo.cell_w, CELL_H, color);
        }
    }

    // ── Month labels (top) ────────────────────────────────────────────────────
    MonthMarker markers[14];
    int marker_count = collect_month_markers(data, lo, markers, 14);
    int label_count  = (int)history_mo;
    if (label_count > marker_count) label_count = marker_count;
    // Draw month labels left-aligned at the start of each month's first column,
    // so they align visually with the separator line on the left.
    if (marker_count > 0) {
        spr.setTextFont(2);
        spr.setTextColor(TFT_WHITE);
        spr.setTextDatum(TL_DATUM);
        for (int mi = 0; mi < marker_count; mi++) {
            int mx = lo.grid_left + markers[mi].week * (lo.cell_w + lo.col_gap);
            spr.drawString(month_abbr(markers[mi].month), mx, UNIT_TOP);
        }
    }

    // ── Month separators (1 px grey vertical lines, 1px pad each side) ────────
    spr.setTextDatum(TL_DATUM);
    for (int mi = 1; mi < marker_count; mi++) {
        // With col_gap=3 the gap is [pad1][line][pad1]; line sits at offset +1
        int x = lo.grid_left + markers[mi].week * (lo.cell_w + lo.col_gap) - 2;
        spr.drawFastVLine(x,
            GRID_TOP - MONTH_SEP_OVERHANG,
            GRID_PIXEL_H + MONTH_SEP_OVERHANG * 2,
            RGB565(0x50, 0x50, 0x50));
    }

    // ── Footer bar ────────────────────────────────────────────────────────────
    const int fy = SCREEN_H - FOOTER_H;
    spr.fillRect(0, fy, SCREEN_W, FOOTER_H, TFT_BLACK);

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

