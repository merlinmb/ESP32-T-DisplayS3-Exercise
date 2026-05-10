#include "display_grid.h"
#include "ui_fonts.h"
#include <Arduino.h>

#define SCREEN_W     320
#define SCREEN_H     170  // T-Display S3 landscape height (172 on the old C6 board)
#define CELL_H       12
#define MIN_COL_GAP  1
#define ROW_GAP      2
#define FOOTER_H     28
#define SIDE_PAD     3
#define LABEL_FONT_H 18
#define LABEL_GAP    6
#define GRID_PIXEL_H ((GRID_DAYS * CELL_H) + ((GRID_DAYS - 1) * ROW_GAP))
#define GRID_UNIT_H  (LABEL_FONT_H + LABEL_GAP + GRID_PIXEL_H)
#define UNIT_TOP     ((SCREEN_H - FOOTER_H - GRID_UNIT_H) / 2)
#define GRID_TOP     (UNIT_TOP + LABEL_FONT_H + LABEL_GAP)
#define MONTH_SEP_OVERHANG 5
#define MONTH_SEP_WIDTH    1
#define MONTH_SEP_PAD      2
#define MONTH_SEP_MASK_W   (MONTH_SEP_WIDTH + (MONTH_SEP_PAD * 2))
#define MONTH_LABEL_COUNT kMaxHistoryMonths
#define MONTH_SEP_COUNT   13  // max month boundaries in 53 weeks

// Exercise load colour palette (matches strava_bridge thresholds)
// Level 0 = no activity  1 = low (<50 min)  2 = medium (<100)  3 = high (<150)  4 = very high
static const lv_color_t LEVEL_COLORS[5] = {
    lv_color_hex(0x171b20), // 0  grey  - no activity
    lv_color_hex(0x27ae60), // 1  green - low
    lv_color_hex(0xf39c12), // 2  amber - medium
    lv_color_hex(0xe67e22), // 3  orange - high
    lv_color_hex(0xe74c3c), // 4  red   - very high
};
static const lv_color_t LEVEL_BRIGHT[5] = {
    lv_color_hex(0x171b20), // 0  never animates
    lv_color_hex(0x2ecc71), // 1  bright green
    lv_color_hex(0xf1c40f), // 2  yellow
    lv_color_hex(0xf0932b), // 3  bright orange
    lv_color_hex(0xff5252), // 4  bright red
};

struct GridLayout {
    int visible_weeks;
    int grid_left;
    int cell_w;
    int col_gap;
    int sep_before_week[GRID_WEEKS];
};

static lv_obj_t *s_screens[GRID_METRIC_COUNT];
static lv_obj_t *s_cells[GRID_METRIC_COUNT][GRID_WEEKS][GRID_DAYS];
static lv_obj_t *s_month_labels[GRID_METRIC_COUNT][MONTH_LABEL_COUNT];
static lv_obj_t *s_month_seps[GRID_METRIC_COUNT][MONTH_SEP_COUNT];
static GridLayout s_layouts[GRID_METRIC_COUNT];

struct MonthMarker {
    int week;
    int month;
};

static int collect_month_markers(const StravaData &data, const GridLayout &layout,
                                 MonthMarker *markers, int max_markers);

// Store level per cell for animation callback lookup
static uint8_t s_cell_levels[GRID_METRIC_COUNT][GRID_WEEKS][GRID_DAYS];

static bool metric_valid(GridMetric metric) {
    return metric >= GRID_METRIC_LOAD && metric < GRID_METRIC_COUNT;
}

static float metric_value(const ExerciseDay &day, GridMetric metric) {
    return (metric == GRID_METRIC_CALORIES) ? day.calories : day.load;
}

static uint8_t metric_level(const ExerciseDay &day, GridMetric metric) {
    return (metric == GRID_METRIC_CALORIES) ? day.calories_level : day.load_level;
}

// Animation callback: val 0-255, interpolates base->bright
static void anim_color_cb(void *obj, int32_t val) {
    lv_obj_t *cell = (lv_obj_t *)obj;
    uint8_t lvl = (uint8_t)(uintptr_t)lv_obj_get_user_data(cell);
    if (lvl >= 5) lvl = 4;
    lv_color_t mixed = lv_color_mix(LEVEL_BRIGHT[lvl], LEVEL_COLORS[lvl], (uint8_t)val);
    lv_obj_set_style_bg_color(cell, mixed, 0);
}

static void anim_bg_opa_cb(void *obj, int32_t val) {
    lv_obj_t *cell = (lv_obj_t *)obj;
    uint8_t opa = (val < 0) ? 0 : (val > LV_OPA_COVER ? LV_OPA_COVER : (uint8_t)val);
    lv_obj_set_style_bg_opa(cell, opa, 0);
}

static void anim_border_opa_cb(void *obj, int32_t val) {
    lv_obj_t *cell = (lv_obj_t *)obj;
    uint8_t opa = (val < 0) ? 0 : (val > LV_OPA_COVER ? LV_OPA_COVER : (uint8_t)val);
    lv_obj_set_style_border_opa(cell, opa, 0);
}

static void civil_from_days(int32_t days_since_epoch, int &year, int &month, int &day) {
    int32_t z = days_since_epoch + 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = (int)(yoe + era * 400);
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    day = (int)(doy - (153 * mp + 2) / 5 + 1);
    month = (int)(mp + (mp < 10 ? 3 : -9));
    year += (month <= 2);
}

static int32_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= (int)(month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2u ? (unsigned)-3 : 9u)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int)doe - 719468;
}

static uint8_t weekday_sun0(int32_t d) {
    int w = (int)((d + 4) % 7);
    if (w < 0) w += 7;
    return (uint8_t)w;
}

static uint8_t clamp_history_months(uint8_t months) {
    if (months < kMinHistoryMonths) return kDefaultHistoryMonths;
    if (months > kMaxHistoryMonths) return kMaxHistoryMonths;
    return months;
}

static int data_week_count(const StravaData &data) {
    return (data.week_count > GRID_WEEKS) ? GRID_WEEKS : data.week_count;
}

static int approximate_visible_weeks(uint8_t history_months) {
    int weeks = (int)((history_months * 31u + 6u) / 7u);
    if (weeks < 1) weeks = 1;
    if (weeks > GRID_WEEKS) weeks = GRID_WEEKS;
    return weeks;
}

static void shift_months(int &year, int &month, int delta) {
    month += delta;
    while (month < 1) {
        month += 12;
        year--;
    }
    while (month > 12) {
        month -= 12;
        year++;
    }
}

static int visible_week_count_for_history(const StravaData &data, uint8_t history_months) {
    const int fallback_weeks = approximate_visible_weeks(history_months);
    if (data.anchor_week_start_days == 0) return fallback_weeks;

    int ref_year = 0;
    int ref_month = 0;
    int ref_day = 0;
    int32_t ref_days = (data.current_day_days != 0)
        ? data.current_day_days
        : ((data.latest_data_day_days != 0)
            ? data.latest_data_day_days
            : (data.anchor_week_start_days + 6));
    civil_from_days(ref_days, ref_year, ref_month, ref_day);
    shift_months(ref_year, ref_month, 1 - (int)history_months);

    int32_t start_of_month = days_from_civil(ref_year, (unsigned)ref_month, 1);
    int32_t first_visible_sunday = start_of_month - weekday_sun0(start_of_month);
    int32_t delta_days = data.anchor_week_start_days - first_visible_sunday;
    if (delta_days < 0) return fallback_weeks;

    int weeks = (int)(delta_days / 7) + 1;
    int available_weeks = data_week_count(data);
    if (available_weeks > 0 && weeks > available_weeks) weeks = available_weeks;
    if (weeks < 1) weeks = 1;
    if (weeks > GRID_WEEKS) weeks = GRID_WEEKS;
    return weeks;
}

static GridLayout build_grid_layout(const StravaData &data, const Config &cfg) {
    GridLayout layout{};
    layout.visible_weeks = visible_week_count_for_history(data, clamp_history_months(cfg.history_months));
    layout.col_gap = MIN_COL_GAP;

    MonthMarker markers[MONTH_SEP_COUNT];
    int marker_count = collect_month_markers(data, layout, markers, MONTH_SEP_COUNT);
    int extra_sep_w = MONTH_SEP_MASK_W - layout.col_gap;
    if (extra_sep_w < 0) extra_sep_w = 0;

    int extra_sep_total_w = 0;
    for (int marker_idx = 1; marker_idx < marker_count; marker_idx++) {
        int week = markers[marker_idx].week;
        if (week <= 0 || week >= layout.visible_weeks) continue;
        extra_sep_total_w += extra_sep_w;
        for (int w = week; w < layout.visible_weeks; w++) {
            layout.sep_before_week[w] += extra_sep_w;
        }
    }

    int available_w = SCREEN_W - (SIDE_PAD * 2);
    int total_gap_w = (layout.visible_weeks - 1) * layout.col_gap;
    layout.cell_w = (available_w - total_gap_w - extra_sep_total_w) / layout.visible_weeks;
    if (layout.cell_w < 1) layout.cell_w = 1;

    int grid_pixel_w = layout.visible_weeks * layout.cell_w + total_gap_w + extra_sep_total_w;
    layout.grid_left = SIDE_PAD + ((available_w - grid_pixel_w) / 2);
    return layout;
}

static int first_visible_week_index(int total_weeks, int visible_weeks) {
    if (visible_weeks >= total_weeks) return 0;
    return total_weeks - visible_weeks;
}

static int week_x(const GridLayout &layout, int week) {
    return layout.grid_left
        + week * (layout.cell_w + layout.col_gap)
        + layout.sep_before_week[week];
}

static int month_separator_x(const GridLayout &layout, int week) {
    return week_x(layout, week) - MONTH_SEP_MASK_W;
}

static bool current_day_display_slot(const StravaData &data, const GridLayout &layout,
                                     int &display_week, int &day_index) {
    display_week = -1;
    day_index = -1;

    if (data.current_day_days == 0 || data.anchor_week_start_days == 0) return false;

    int total_weeks = data_week_count(data);
    if (total_weeks <= 0) return false;

    day_index = weekday_sun0(data.current_day_days);
    int32_t current_week_start = data.current_day_days - day_index;
    int32_t delta_days = data.anchor_week_start_days - current_week_start;
    if (delta_days < 0 || (delta_days % 7) != 0) {
        display_week = -1;
        day_index = -1;
        return false;
    }

    int week_diff = (int)(delta_days / 7);
    int data_week = (total_weeks - 1) - week_diff;
    int first_week = first_visible_week_index(total_weeks, layout.visible_weeks);
    display_week = data_week - first_week;
    if (display_week < 0 || display_week >= layout.visible_weeks) {
        display_week = -1;
        day_index = -1;
        return false;
    }

    return true;
}

static const char *month_abbr(int month) {
    static const char *MONTHS[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (month < 1 || month > 12) return "";
    return MONTHS[month - 1];
}

static int collect_month_markers(const StravaData &data, const GridLayout &layout,
                                 MonthMarker *markers, int max_markers) {
    if (data.anchor_week_start_days == 0 || !markers || max_markers <= 0) return 0;

    int total_weeks = data_week_count(data);
    if (total_weeks <= 0) total_weeks = GRID_WEEKS;
    int start_week = first_visible_week_index(total_weeks, layout.visible_weeks);

    int marker_count = 0;
    int prev_month = -1;
    for (int visible_w = 0; visible_w < layout.visible_weeks && marker_count < max_markers; visible_w++) {
        int data_week = start_week + visible_w;
        int32_t week_sun = data.anchor_week_start_days - (total_weeks - 1 - data_week) * 7;
        int year = 0;
        int month = 0;
        int day = 0;
        civil_from_days(week_sun + 3, year, month, day); // midweek sample
        if (month != prev_month) {
            markers[marker_count].week = visible_w;
            markers[marker_count].month = month;
            marker_count++;
            prev_month = month;
        }
    }

    return marker_count;
}

static bool ensure_grid_cells(GridMetric metric, int week_count) {
    if (!metric_valid(metric) || !s_screens[metric]) return false;

    if (week_count < 0) week_count = 0;
    if (week_count > GRID_WEEKS) week_count = GRID_WEEKS;

    lv_obj_t *screen = s_screens[metric];
    for (int w = 0; w < week_count; w++) {
        for (int d = 0; d < GRID_DAYS; d++) {
            if (s_cells[metric][w][d]) continue;

            lv_obj_t *cell = lv_obj_create(screen);
            if (!cell) {
                Serial.printf("[Grid:%d] Failed to create cell w=%d d=%d\n", (int)metric, w, d);
                return false;
            }

            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, s_layouts[metric].cell_w, CELL_H);
            lv_obj_set_style_radius(cell, 1, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_border_color(cell, lv_color_white(), 0);
            lv_obj_set_style_border_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_style_bg_color(cell, LEVEL_COLORS[0], 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_user_data(cell, (void *)(uintptr_t)0);
            s_cells[metric][w][d] = cell;
            s_cell_levels[metric][w][d] = 0;
        }
    }

    return true;
}

static bool ensure_month_labels(GridMetric metric, int label_count) {
    if (!metric_valid(metric) || !s_screens[metric]) return false;

    if (label_count < 0) label_count = 0;
    if (label_count > MONTH_LABEL_COUNT) label_count = MONTH_LABEL_COUNT;

    const lv_color_t label_color = lv_color_white();
    lv_obj_t *screen = s_screens[metric];
    for (int i = 0; i < label_count; i++) {
        if (s_month_labels[metric][i]) continue;

        lv_obj_t *lbl = lv_label_create(screen);
        if (!lbl) {
            Serial.printf("[Grid:%d] Failed to create month label %d\n", (int)metric, i);
            return false;
        }

        lv_obj_set_style_text_font(lbl, ui_font_label(), 0);
        lv_obj_set_style_text_color(lbl, label_color, 0);
        lv_label_set_text(lbl, "");
        lv_obj_set_pos(lbl, s_layouts[metric].grid_left, UNIT_TOP);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        s_month_labels[metric][i] = lbl;
    }

    return true;
}

static bool ensure_month_separators(GridMetric metric, int sep_count) {
    if (!metric_valid(metric) || !s_screens[metric]) return false;

    if (sep_count < 0) sep_count = 0;
    if (sep_count > MONTH_SEP_COUNT) sep_count = MONTH_SEP_COUNT;

    lv_obj_t *screen = s_screens[metric];
    for (int i = 0; i < sep_count; i++) {
        if (s_month_seps[metric][i]) continue;

        lv_obj_t *sep_mask = lv_obj_create(screen);
        if (!sep_mask) {
            Serial.printf("[Grid:%d] Failed to create month separator %d\n", (int)metric, i);
            return false;
        }

        lv_obj_remove_style_all(sep_mask);
        lv_obj_set_size(sep_mask, MONTH_SEP_MASK_W, GRID_PIXEL_H + (MONTH_SEP_OVERHANG * 2));
        lv_obj_set_style_bg_color(sep_mask, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(sep_mask, LV_OPA_COVER, 0);
        lv_obj_set_pos(sep_mask, 0, GRID_TOP - MONTH_SEP_OVERHANG);
        lv_obj_add_flag(sep_mask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(sep_mask, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *sep_line = lv_obj_create(sep_mask);
        if (!sep_line) {
            Serial.printf("[Grid:%d] Failed to create month separator line %d\n", (int)metric, i);
            return false;
        }

        lv_obj_remove_style_all(sep_line);
        lv_obj_set_size(sep_line, MONTH_SEP_WIDTH, GRID_PIXEL_H + (MONTH_SEP_OVERHANG * 2));
        lv_obj_set_style_bg_color(sep_line, lv_color_hex(0xB3B3B3), 0);
        lv_obj_set_style_bg_opa(sep_line, LV_OPA_COVER, 0);
        lv_obj_set_pos(sep_line, MONTH_SEP_PAD, 0);
        lv_obj_clear_flag(sep_line, LV_OBJ_FLAG_SCROLLABLE);
        s_month_seps[metric][i] = sep_mask;
    }

    return true;
}

static void apply_grid_layout(GridMetric metric, const GridLayout &layout) {
    s_layouts[metric] = layout;

    for (int w = 0; w < GRID_WEEKS; w++) {
        bool visible = (w < layout.visible_weeks);
        for (int d = 0; d < GRID_DAYS; d++) {
            if (!s_cells[metric][w][d]) continue;

            if (!visible) {
                lv_obj_add_flag(s_cells[metric][w][d], LV_OBJ_FLAG_HIDDEN);
                continue;
            }

            int x = week_x(layout, w);
            int y = GRID_TOP + d * (CELL_H + ROW_GAP);
            lv_obj_set_size(s_cells[metric][w][d], layout.cell_w, CELL_H);
            lv_obj_set_pos(s_cells[metric][w][d], x, y);
            lv_obj_clear_flag(s_cells[metric][w][d], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_month_labels(GridMetric metric, const StravaData &data, const Config &cfg, const GridLayout &layout) {
    int label_count = clamp_history_months(cfg.history_months);
    if (!ensure_month_labels(metric, label_count)) return;

    for (int i = 0; i < MONTH_LABEL_COUNT; i++) {
        if (!s_month_labels[metric][i]) continue;
        lv_label_set_text(s_month_labels[metric][i], "");
        lv_obj_add_flag(s_month_labels[metric][i], LV_OBJ_FLAG_HIDDEN);
    }

    MonthMarker markers[MONTH_SEP_COUNT];
    int marker_count = collect_month_markers(data, layout, markers, MONTH_SEP_COUNT);
    if (marker_count <= 0) return;

    if (label_count > marker_count) label_count = marker_count;
    if (label_count < 1) label_count = 1;

    for (int i = 0; i < label_count; i++) {
        int marker_idx = i;
        if (marker_count > label_count && label_count > 1) {
            marker_idx = (i * (marker_count - 1) + (label_count - 1) / 2) / (label_count - 1);
        }

        const char *text = month_abbr(markers[marker_idx].month);
    int marker_x = week_x(layout, markers[marker_idx].week);

        lv_label_set_text(s_month_labels[metric][i], text);
        lv_obj_update_layout(s_month_labels[metric][i]);

        int x = month_separator_x(layout, markers[marker_idx].week);
        if (markers[marker_idx].week == 0 && x < layout.grid_left) x = layout.grid_left;
        if (x < 0) x = 0;
        int max_x = SCREEN_W - lv_obj_get_width(s_month_labels[metric][i]);
        if (x > max_x) x = max_x;

        lv_obj_set_pos(s_month_labels[metric][i], x, UNIT_TOP);
        lv_obj_clear_flag(s_month_labels[metric][i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_month_separators(GridMetric metric, const StravaData &data, const GridLayout &layout) {
    MonthMarker markers[MONTH_SEP_COUNT];
    int marker_count = collect_month_markers(data, layout, markers, MONTH_SEP_COUNT);
    int sep_count = marker_count - 1;
    if (sep_count < 0) sep_count = 0;
    if (!ensure_month_separators(metric, sep_count)) return;

    for (int i = 0; i < MONTH_SEP_COUNT; i++) {
        if (s_month_seps[metric][i]) lv_obj_add_flag(s_month_seps[metric][i], LV_OBJ_FLAG_HIDDEN);
    }
    if (data.anchor_week_start_days == 0) return;

    for (int marker_idx = 1; marker_idx < marker_count; marker_idx++) {
        int x = month_separator_x(layout, markers[marker_idx].week);
        lv_obj_set_pos(s_month_seps[metric][marker_idx - 1], x, GRID_TOP - MONTH_SEP_OVERHANG);
        lv_obj_clear_flag(s_month_seps[metric][marker_idx - 1], LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *display_grid_build(GridMetric metric, const char *title, const Config &cfg) {
    if (!metric_valid(metric)) return nullptr;

    GridLayout initial_layout = build_grid_layout(StravaData{}, cfg);

    memset(s_cells[metric], 0, sizeof(s_cells[metric]));
    memset(s_cell_levels[metric], 0, sizeof(s_cell_levels[metric]));
    memset(s_month_labels[metric], 0, sizeof(s_month_labels[metric]));
    memset(s_month_seps[metric], 0, sizeof(s_month_seps[metric]));
    s_layouts[metric] = initial_layout;

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_screens[metric] = screen;

    const lv_color_t label_color = lv_color_white();

    if (!ensure_month_labels(metric, clamp_history_months(cfg.history_months))) {
        return nullptr;
    }

    // Grid cells are created only for the visible history window and expanded on demand.
    if (!ensure_grid_cells(metric, initial_layout.visible_weeks)) {
        return nullptr;
    }

    apply_grid_layout(metric, initial_layout);
    update_month_labels(metric, StravaData{}, cfg, initial_layout);
    update_month_separators(metric, StravaData{}, initial_layout);

    // Footer title directly on the screen to avoid an extra container object.
    lv_obj_t *user_lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(user_lbl, ui_font_label(), 0);
    lv_obj_set_style_text_color(user_lbl, lv_color_hex(0xfc4c02), 0); // Strava orange
    lv_label_set_text(user_lbl, title ? title : "Exercise Load");
    lv_obj_align(user_lbl, LV_ALIGN_BOTTOM_LEFT, SIDE_PAD, -5);

    // Legend: 5 squares right-aligned, then "More" label
    // Build right-to-left: "More" label, then squares
    lv_obj_t *more_lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(more_lbl, ui_font_label(), 0);
    lv_obj_set_style_text_color(more_lbl, label_color, 0);
    lv_label_set_text(more_lbl, "More");
    lv_obj_align(more_lbl, LV_ALIGN_BOTTOM_RIGHT, -SIDE_PAD, -5);

    // Place squares left of "More" label
    // Each square is 5px wide + 2px gap
    for (int i = 4; i >= 0; i--) {
        lv_obj_t *sq = lv_obj_create(screen);
        lv_obj_remove_style_all(sq);
        lv_obj_set_size(sq, 5, CELL_H);
        lv_obj_set_style_radius(sq, 1, 0);
        lv_obj_set_style_border_width(sq, 0, 0);
        lv_obj_set_style_pad_all(sq, 0, 0);
        lv_obj_set_style_bg_color(sq, LEVEL_COLORS[i], 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        // Position: right edge of "More" text minus offset per square
        // "More" text ~24px wide, right edge at 320-8=312, so More starts at ~288
        // squares at: 288 - 2(gap) - 5(sq) = 281, then 281-7=274, etc.
        int x_right = SCREEN_W - SIDE_PAD - 40; // approximate left edge of the larger "More" label
        int sq_x = x_right - (4 - i + 1) * (5 + 2);
        lv_obj_set_pos(sq, sq_x, SCREEN_H - FOOTER_H + ((FOOTER_H - CELL_H) / 2));
    }

    return screen;
}

void display_grid_stop_animations(GridMetric metric) {
    if (!metric_valid(metric) || !s_screens[metric]) return;

    for (int w = 0; w < GRID_WEEKS; w++) {
        for (int d = 0; d < GRID_DAYS; d++) {
            if (s_cells[metric][w][d]) {
                lv_anim_delete(s_cells[metric][w][d], anim_color_cb);
                lv_anim_delete(s_cells[metric][w][d], anim_bg_opa_cb);
                lv_anim_delete(s_cells[metric][w][d], anim_border_opa_cb);
                // Restore base colour
                uint8_t lvl = s_cell_levels[metric][w][d];
                lv_obj_set_style_bg_color(s_cells[metric][w][d], LEVEL_COLORS[lvl], 0);
                lv_obj_set_style_bg_opa(s_cells[metric][w][d], LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(s_cells[metric][w][d], 0, 0);
                lv_obj_set_style_border_opa(s_cells[metric][w][d], LV_OPA_TRANSP, 0);
            }
        }
    }
}

void display_grid_update(GridMetric metric, const StravaData &data, const Config &cfg) {
    if (!metric_valid(metric) || !s_screens[metric]) return;

    Serial.printf("[Grid:%d] update: valid=%d week_count=%d anchor=%ld\n",
                  (int)metric, (int)data.valid, (int)data.week_count, (long)data.anchor_week_start_days);

    display_grid_stop_animations(metric);
    GridLayout layout = build_grid_layout(data, cfg);
    if (!ensure_grid_cells(metric, layout.visible_weeks)) return;
    apply_grid_layout(metric, layout);
    update_month_labels(metric, data, cfg, layout);
    update_month_separators(metric, data, layout);

    int current_display_week = -1;
    int current_day_index = -1;
    bool has_current_day = current_day_display_slot(data, layout, current_display_week, current_day_index);

    const int total_weeks = data_week_count(data);
    const int first_week = first_visible_week_index(total_weeks, layout.visible_weeks);
    const int week_count = (total_weeks > layout.visible_weeks) ? layout.visible_weeks : total_weeks;

    int cells_colored = 0;
    for (int display_w = 0; display_w < GRID_WEEKS; display_w++) {
        for (int d = 0; d < GRID_DAYS; d++) {
            int data_w = first_week + display_w;
            bool in_range = (display_w < layout.visible_weeks) && (display_w < week_count);
            float   ld  = in_range ? metric_value(data.days[data_w][d], metric) : 0.0f;
            uint8_t lvl = in_range ? metric_level(data.days[data_w][d], metric) : 0;
            s_cell_levels[metric][display_w][d] = lvl;

            if (!s_cells[metric][display_w][d]) continue;

            lv_obj_set_user_data(s_cells[metric][display_w][d], (void *)(uintptr_t)lvl);
            lv_obj_set_style_bg_color(s_cells[metric][display_w][d], LEVEL_COLORS[lvl], 0);
            lv_obj_set_style_bg_opa(s_cells[metric][display_w][d], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_cells[metric][display_w][d], 0, 0);
            lv_obj_set_style_border_opa(s_cells[metric][display_w][d], LV_OPA_TRANSP, 0);
            if (lvl > 0 && display_w < layout.visible_weeks) cells_colored++;
        }
    }
    Serial.printf("[Grid:%d] active days=%d, visible_weeks=%d\n", (int)metric, cells_colored, layout.visible_weeks);
    Serial.printf("[Grid:%d] cells_colored=%d (lvl>0) out of %d total\n",
                  (int)metric, cells_colored, layout.visible_weeks * GRID_DAYS);
}

void display_grid_start_animations(GridMetric metric, const StravaData &data, const Config &cfg) {
    if (!metric_valid(metric) || !s_screens[metric]) return;

    display_grid_stop_animations(metric);

    const GridLayout &layout = s_layouts[metric];
    int current_display_week = -1;
    int current_day_index = -1;
    bool has_current_day = current_day_display_slot(data, layout, current_display_week, current_day_index);

    const int total_weeks = data_week_count(data);
    const int first_week = first_visible_week_index(total_weeks, layout.visible_weeks);
    const int week_count = (total_weeks > layout.visible_weeks) ? layout.visible_weeks : total_weeks;

    // Collect non-zero values to compute the percentile threshold for animation.
    float loads[GRID_WEEKS * GRID_DAYS];
    int load_n = 0;
    for (int w = 0; w < week_count; w++) {
        int data_w = first_week + w;
        for (int d = 0; d < GRID_DAYS; d++) {
            float value = metric_value(data.days[data_w][d], metric);
            if (value > 0.0f)
                loads[load_n++] = value;
        }
    }

    // Insertion sort ascending (max 371 items)
    for (int i = 1; i < load_n; i++) {
        float key = loads[i];
        int j = i - 1;
        while (j >= 0 && loads[j] > key) { loads[j+1] = loads[j]; j--; }
        loads[j+1] = key;
    }

    float threshold = 1e9f;
    if (load_n > 0 && cfg.anim_top_pct > 0) {
        int idx = (int)(load_n * (100 - cfg.anim_top_pct) / 100.0f);
        if (idx >= load_n) idx = load_n - 1;
        threshold = loads[idx];
        if (threshold <= 0.0f) threshold = 1.0f;
    }

    for (int display_w = 0; display_w < GRID_WEEKS; display_w++) {
        for (int d = 0; d < GRID_DAYS; d++) {
            if (!s_cells[metric][display_w][d]) continue;

            int data_w = first_week + display_w;
            bool in_range = (display_w < layout.visible_weeks) && (display_w < week_count);
            float ld = in_range ? metric_value(data.days[data_w][d], metric) : 0.0f;

            bool is_current_day = has_current_day
                && display_w == current_display_week
                && d == current_day_index;

            if (ld > 0.0f && ld >= threshold && !is_current_day) {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_cells[metric][display_w][d]);
                lv_anim_set_exec_cb(&a, anim_color_cb);
                lv_anim_set_values(&a, 0, 255);
                lv_anim_set_duration(&a, cfg.anim_period_ms);
                lv_anim_set_playback_duration(&a, cfg.anim_period_ms);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
                lv_anim_set_delay(&a, (uint32_t)random(0, (long)cfg.anim_period_ms));
                lv_anim_start(&a);
            }

            if (is_current_day) {
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, s_cells[metric][display_w][d]);

                uint8_t lvl = s_cell_levels[metric][display_w][d];
                if (lvl == 0) {
                    lv_obj_set_style_border_width(s_cells[metric][display_w][d], 1, 0);
                    lv_obj_set_style_border_color(s_cells[metric][display_w][d], lv_color_white(), 0);
                    lv_anim_set_exec_cb(&a, anim_border_opa_cb);
                } else {
                    lv_anim_set_exec_cb(&a, anim_bg_opa_cb);
                }

                lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
                lv_anim_set_duration(&a, cfg.anim_period_ms);
                lv_anim_set_playback_duration(&a, cfg.anim_period_ms);
                lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                lv_anim_set_path_cb(&a, lv_anim_path_linear);
                lv_anim_start(&a);
            }
        }
    }
}
