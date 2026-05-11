#include "display_calorie_trend.h"
#include "ui_fonts.h"

#include <Arduino.h>
#include <math.h>

namespace {

constexpr int kScreenW   = 320;
constexpr int kScreenH   = 170;

// Chart card — wider and taller, pushed left/up
constexpr int kChartW    = 218;
constexpr int kChartH    = 140;
constexpr int kChartX    = -8;
constexpr int kChartY    = 1;

// Summary card — expanded to use available screen space
constexpr int kSummaryW  = 84;
constexpr int kSummaryH  = 140;
constexpr int kSummaryX  = 230;
constexpr int kSummaryY  = 6;

// Sparkline geometry (coords within chart card)
constexpr int kPtCount   = 7;
constexpr int kSparkLeft = 7;
constexpr int kSparkRight= 189;
constexpr int kSparkTop  = 4;
constexpr int kSparkBot  = 94;
constexpr int kBaseline  = 99;
constexpr int kDotD      = 7;   // regular dot diameter
constexpr int kTodayDotD = 11;  // emphasize most recent day
constexpr int kCurveSamplesPerSegment = 10;
constexpr int kCurvePtCount = (kPtCount - 1) * kCurveSamplesPerSegment + 1;

static lv_obj_t  *s_screen     = nullptr;
static lv_obj_t  *s_spark_line = nullptr;
static lv_obj_t  *s_dots[kPtCount];
static lv_obj_t  *s_day_labels[kPtCount];
static lv_obj_t  *s_value      = nullptr;
static lv_obj_t  *s_delta_wrap = nullptr;
static lv_obj_t  *s_delta_up[3];
static lv_obj_t  *s_delta_down[3];
static lv_obj_t  *s_delta_flat = nullptr;
static lv_obj_t  *s_title      = nullptr;
static lv_point_precise_t s_pts[kPtCount];
static lv_point_precise_t s_curve_pts[kCurvePtCount];

static const char *weekday_short(uint8_t d) {
    static const char *kL[7] = {"S","M","T","W","T","F","S"};
    return kL[(d < 7) ? d : 0];
}

static uint8_t weekday_sun0(int32_t days) {
    int w = (int)((days + 4) % 7);
    if (w < 0) w += 7;
    return (uint8_t)w;
}

static bool day_calories(const StravaData &data, int32_t day_epoch, float &cal) {
    if (data.anchor_week_start_days == 0) return false;
    uint8_t wd    = weekday_sun0(day_epoch);
    int32_t ws    = day_epoch - wd;
    int32_t delta = data.anchor_week_start_days - ws;
    if (delta < 0 || (delta % 7) != 0) return false;
    int wk = (int)(delta / 7);
    if (wk >= GRID_WEEKS) return false;
    cal = data.days[(GRID_WEEKS - 1) - wk][wd].calories;
    return true;
}

static int pt_x(int i) {
    return kSparkLeft + (i * (kSparkRight - kSparkLeft)) / (kPtCount - 1);
}

static int pt_y(float norm) {
    return kSparkTop + (int)lroundf((1.0f - norm) * (float)(kSparkBot - kSparkTop));
}

static void rebuild_smooth_curve_points() {
    int out_idx = 0;

    for (int i = 0; i < (kPtCount - 1); ++i) {
        const lv_point_precise_t p0 = s_pts[(i == 0) ? 0 : (i - 1)];
        const lv_point_precise_t p1 = s_pts[i];
        const lv_point_precise_t p2 = s_pts[i + 1];
        const lv_point_precise_t p3 = s_pts[(i + 2 < kPtCount) ? (i + 2) : (kPtCount - 1)];

        const float cp1x = (float)p1.x + ((float)p2.x - (float)p0.x) / 6.0f;
        const float cp1y = (float)p1.y + ((float)p2.y - (float)p0.y) / 6.0f;
        const float cp2x = (float)p2.x - ((float)p3.x - (float)p1.x) / 6.0f;
        const float cp2y = (float)p2.y - ((float)p3.y - (float)p1.y) / 6.0f;

        for (int s = 0; s < kCurveSamplesPerSegment; ++s) {
            const float t = (float)s / (float)kCurveSamplesPerSegment;
            const float u = 1.0f - t;

            const float bx =
                u * u * u * (float)p1.x +
                3.0f * u * u * t * cp1x +
                3.0f * u * t * t * cp2x +
                t * t * t * (float)p2.x;
            const float by =
                u * u * u * (float)p1.y +
                3.0f * u * u * t * cp1y +
                3.0f * u * t * t * cp2y +
                t * t * t * (float)p2.y;

            s_curve_pts[out_idx].x = (lv_value_precise_t)lroundf(bx);
            s_curve_pts[out_idx].y = (lv_value_precise_t)lroundf(by);
            out_idx++;
        }
    }

    s_curve_pts[out_idx] = s_pts[kPtCount - 1];
}

static void style_delta_segment(lv_obj_t *obj, lv_color_t color) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void set_delta_marker(float delta) {
    if (!s_delta_wrap || !s_delta_flat) return;

    const lv_color_t up_color = lv_color_hex(0x32cd32);
    const lv_color_t down_color = lv_color_hex(0xff4d4f);
    const lv_color_t flat_color = lv_color_hex(0xb0bec5);

    if (delta > 0.5f) {
        for (int i = 0; i < 3; ++i) {
            lv_obj_clear_flag(s_delta_up[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_delta_down[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_delta_up[i], up_color, 0);
        }
        lv_obj_add_flag(s_delta_flat, LV_OBJ_FLAG_HIDDEN);
    } else if (delta < -0.5f) {
        for (int i = 0; i < 3; ++i) {
            lv_obj_add_flag(s_delta_up[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_delta_down[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_delta_down[i], down_color, 0);
        }
        lv_obj_add_flag(s_delta_flat, LV_OBJ_FLAG_HIDDEN);
    } else {
        for (int i = 0; i < 3; ++i) {
            lv_obj_add_flag(s_delta_up[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_delta_down[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(s_delta_flat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_delta_flat, flat_color, 0);
    }
}

} // namespace

lv_obj_t *display_calorie_trend_build(const char *title) {
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_size(s_screen, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *chart_card = lv_obj_create(s_screen);
    lv_obj_set_size(chart_card, kChartW, kChartH);
    lv_obj_set_pos(chart_card, kChartX, kChartY);
    lv_obj_set_style_radius(chart_card, 22, 0);
    lv_obj_set_style_bg_color(chart_card, lv_color_hex(0x0b131a), 0);
    lv_obj_set_style_bg_opa(chart_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart_card, 1, 0);
    lv_obj_set_style_border_color(chart_card, lv_color_hex(0x1f2d39), 0);
    lv_obj_clear_flag(chart_card, LV_OBJ_FLAG_SCROLLABLE);

    // Subtle horizontal guide lines
    const int kSpan = kSparkRight - kSparkLeft;
    for (int i = 0; i < 3; ++i) {
        int gy = kSparkTop + i * (kSparkBot - kSparkTop) / 2;
        lv_obj_t *g = lv_obj_create(chart_card);
        lv_obj_remove_style_all(g);
        lv_obj_set_size(g, kSpan, 1);
        lv_obj_set_style_bg_color(g, lv_color_hex(0x173042), 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_40, 0);
        lv_obj_set_pos(g, kSparkLeft, gy);
    }

    // Baseline
    lv_obj_t *baseline = lv_obj_create(chart_card);
    lv_obj_remove_style_all(baseline);
    lv_obj_set_size(baseline, kSpan, 2);
    lv_obj_set_style_bg_color(baseline, lv_color_hex(0x2d4658), 0);
    lv_obj_set_style_bg_opa(baseline, LV_OPA_70, 0);
    lv_obj_set_pos(baseline, kSparkLeft, kBaseline);

    // Sparkline (lv_line) — init with flat midline placeholder
    for (int i = 0; i < kPtCount; ++i) {
        s_pts[i].x = pt_x(i);
        s_pts[i].y = pt_y(0.5f);
    }
    rebuild_smooth_curve_points();
    s_spark_line = lv_line_create(chart_card);
    lv_line_set_points(s_spark_line, s_curve_pts, kCurvePtCount);
    lv_obj_set_style_line_color(s_spark_line, lv_color_hex(0x4cc9f0), 0);
    lv_obj_set_style_line_width(s_spark_line, 2, 0);
    lv_obj_set_style_line_rounded(s_spark_line, true, 0);
    lv_obj_set_pos(s_spark_line, 0, 0);

    // Dot markers + weekday labels
    for (int i = 0; i < kPtCount; ++i) {
        const int dot_d = (i == (kPtCount - 1)) ? kTodayDotD : kDotD;
        s_dots[i] = lv_obj_create(chart_card);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], dot_d, dot_d);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(0x4cc9f0), 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(s_dots[i], pt_x(i) - dot_d / 2, pt_y(0.5f) - dot_d / 2);

        s_day_labels[i] = lv_label_create(chart_card);
        lv_obj_set_width(s_day_labels[i], 12);
        lv_obj_set_style_text_font(s_day_labels[i], ui_font_label(), 0);
        lv_obj_set_style_text_color(s_day_labels[i], lv_color_hex(0x9cb0c1), 0);
        lv_obj_set_style_text_align(s_day_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(s_day_labels[i], "-");
        lv_obj_set_pos(s_day_labels[i], pt_x(i) - 6, kBaseline + 6);
    }

    // Summary card
    lv_obj_t *summary = lv_obj_create(s_screen);
    lv_obj_set_size(summary, kSummaryW, kSummaryH);
    lv_obj_set_pos(summary, kSummaryX, kSummaryY);
    lv_obj_set_style_radius(summary, 20, 0);
    lv_obj_set_style_bg_color(summary, lv_color_hex(0x111a22), 0);
    lv_obj_set_style_bg_opa(summary, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(summary, 1, 0);
    lv_obj_set_style_border_color(summary, lv_color_hex(0x263746), 0);
    lv_obj_set_style_pad_all(summary, 0, 0);
    lv_obj_clear_flag(summary, LV_OBJ_FLAG_SCROLLABLE);

    // Day header centered at the top of the summary card
    lv_obj_t *vert = lv_label_create(summary);
    lv_obj_set_style_text_font(vert, ui_font_label(), 0);
    lv_obj_set_style_text_color(vert, lv_color_hex(0x7fd6f8), 0);
    lv_obj_set_style_text_align(vert, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(vert, "day - 1");
    lv_obj_align(vert, LV_ALIGN_TOP_MID, 0, 6);

    // Previous-day calorie value centered in the summary card
    s_value = lv_label_create(summary);
    lv_obj_set_style_text_font(s_value, ui_font_stat(), 0);
    lv_obj_set_style_text_color(s_value, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_value, "0");
    lv_obj_align(s_value, LV_ALIGN_CENTER, 0, -12);

    // Day-over-day marker under the value (manual filled triangle/bar)
    s_delta_wrap = lv_obj_create(summary);
    lv_obj_remove_style_all(s_delta_wrap);
    lv_obj_set_size(s_delta_wrap, 16, 10);
    lv_obj_align_to(s_delta_wrap, s_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);
    lv_obj_clear_flag(s_delta_wrap, LV_OBJ_FLAG_SCROLLABLE);

    const int up_w[3] = {5, 9, 13};
    const int down_w[3] = {13, 9, 5};
    for (int i = 0; i < 3; ++i) {
        s_delta_up[i] = lv_obj_create(s_delta_wrap);
        style_delta_segment(s_delta_up[i], lv_color_hex(0x32cd32));
        lv_obj_set_size(s_delta_up[i], up_w[i], 2);
        lv_obj_set_pos(s_delta_up[i], (16 - up_w[i]) / 2, i * 3);

        s_delta_down[i] = lv_obj_create(s_delta_wrap);
        style_delta_segment(s_delta_down[i], lv_color_hex(0xff4d4f));
        lv_obj_set_size(s_delta_down[i], down_w[i], 2);
        lv_obj_set_pos(s_delta_down[i], (16 - down_w[i]) / 2, i * 3);
    }

    s_delta_flat = lv_obj_create(s_delta_wrap);
    style_delta_segment(s_delta_flat, lv_color_hex(0xb0bec5));
    lv_obj_set_size(s_delta_flat, 12, 2);
    lv_obj_set_pos(s_delta_flat, 2, 4);
    set_delta_marker(0.0f);

    lv_obj_t *unit = lv_label_create(summary);
    lv_obj_set_style_text_font(unit, ui_font_label(), 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(0x9cb0c1), 0);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(unit, "k.cal");
    lv_obj_update_layout(s_screen);
    lv_area_t axis_label_area;
    lv_area_t summary_area;
    lv_obj_get_coords(s_day_labels[0], &axis_label_area);
    lv_obj_get_coords(summary, &summary_area);
    const int unit_y = axis_label_area.y1 - summary_area.y1;
    const int unit_x = (kSummaryW - lv_obj_get_width(unit)) / 2;
    lv_obj_set_pos(unit, unit_x, unit_y);

    s_title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_title, ui_font_label(), 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xfc4c02), 0);
    lv_label_set_text(s_title, title ? title : "Calories Trend");
    lv_obj_align(s_title, LV_ALIGN_BOTTOM_MID, 0, -5);

    return s_screen;
}

void display_calorie_trend_update(const StravaData &data) {
    if (!s_screen || !s_value || !s_delta_wrap || !s_spark_line) return;

    int32_t ref = 0;
    if (data.current_day_days != 0)
        ref = data.current_day_days - 1;
    else if (data.latest_data_day_days != 0)
        ref = data.latest_data_day_days;

    float vals[kPtCount] = {};
    float mx = 0.0f;
    for (int i = 0; i < kPtCount; ++i) {
        float cal = 0.0f;
        if (ref != 0) day_calories(data, ref - (kPtCount - 1 - i), cal);
        vals[i] = cal;
        if (cal > mx) mx = cal;
    }
    if (mx < 1.0f) mx = 1.0f;

    for (int i = 0; i < kPtCount; ++i) {
        int px = pt_x(i);
        int py = pt_y(vals[i] / mx);
        const int dot_d = (i == (kPtCount - 1)) ? kTodayDotD : kDotD;
        s_pts[i].x = px;
        s_pts[i].y = py;
        lv_obj_set_pos(s_dots[i], px - dot_d / 2, py - dot_d / 2);
        lv_label_set_text(s_day_labels[i],
                          weekday_short(weekday_sun0(ref - (kPtCount - 1 - i))));
    }
    rebuild_smooth_curve_points();
    lv_line_set_points(s_spark_line, s_curve_pts, kCurvePtCount);

    const float day_minus_1 = vals[kPtCount - 1];
    const float day_minus_2 = vals[kPtCount - 2];
    const float delta = day_minus_1 - day_minus_2;

    lv_label_set_text_fmt(s_value, "%d", (int)lroundf(day_minus_1));
    set_delta_marker(delta);
}
