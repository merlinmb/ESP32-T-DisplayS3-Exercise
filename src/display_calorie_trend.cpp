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
constexpr int kChartX    = 6;
constexpr int kChartY    = 10;

// Summary card — expanded to use available screen space
constexpr int kSummaryW  = 84;
constexpr int kSummaryH  = 140;
constexpr int kSummaryX  = 230;
constexpr int kSummaryY  = 10;

// Sparkline geometry (coords within chart card)
constexpr int kPtCount   = 7;
constexpr int kSparkLeft = 18;
constexpr int kSparkRight= 200;
constexpr int kSparkTop  = 14;
constexpr int kSparkBot  = 104;
constexpr int kBaseline  = 111;
constexpr int kDotD      = 7;   // dot diameter

static lv_obj_t  *s_screen     = nullptr;
static lv_obj_t  *s_spark_line = nullptr;
static lv_obj_t  *s_dots[kPtCount];
static lv_obj_t  *s_day_labels[kPtCount];
static lv_obj_t  *s_value      = nullptr;
static lv_obj_t  *s_title      = nullptr;
static lv_point_precise_t s_pts[kPtCount];
static lv_anim_t  s_today_anim;

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

} // namespace

lv_obj_t *display_calorie_trend_build(const char *title) {
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_size(s_screen, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *glow = lv_obj_create(s_screen);
    lv_obj_remove_style_all(glow);
    lv_obj_set_size(glow, 170, 170);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(0x09273d), 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_20, 0);
    lv_obj_set_pos(glow, -44, -42);

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
    s_spark_line = lv_line_create(chart_card);
    lv_line_set_points(s_spark_line, s_pts, kPtCount);
    lv_obj_set_style_line_color(s_spark_line, lv_color_hex(0x4cc9f0), 0);
    lv_obj_set_style_line_width(s_spark_line, 2, 0);
    lv_obj_set_style_line_rounded(s_spark_line, true, 0);
    lv_obj_set_pos(s_spark_line, 0, 0);

    // Dot markers + weekday labels
    for (int i = 0; i < kPtCount; ++i) {
        s_dots[i] = lv_obj_create(chart_card);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], kDotD, kDotD);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(0x4cc9f0), 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(s_dots[i], pt_x(i) - kDotD / 2, pt_y(0.5f) - kDotD / 2);

        s_day_labels[i] = lv_label_create(chart_card);
        lv_obj_set_width(s_day_labels[i], 12);
        lv_obj_set_style_text_font(s_day_labels[i], ui_font_label(), 0);
        lv_obj_set_style_text_color(s_day_labels[i], lv_color_hex(0x9cb0c1), 0);
        lv_obj_set_style_text_align(s_day_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(s_day_labels[i], "-");
        lv_obj_set_pos(s_day_labels[i], pt_x(i) - 6, kBaseline + 6);
    }

    // Breathing animation on last (most-recent) dot
    lv_anim_init(&s_today_anim);
    lv_anim_set_var(&s_today_anim, s_dots[kPtCount - 1]);
    lv_anim_set_exec_cb(&s_today_anim, [](void *var, int32_t v) {
        lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
    });
    lv_anim_set_values(&s_today_anim, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_time(&s_today_anim, 1400);
    lv_anim_set_playback_time(&s_today_anim, 1400);
    lv_anim_set_repeat_count(&s_today_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_today_anim);

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

    // "yesterday" stacked vertically on the left edge
    lv_obj_t *vert = lv_label_create(summary);
    lv_obj_set_style_text_font(vert, ui_font_label(), 0);
    lv_obj_set_style_text_color(vert, lv_color_hex(0x7fd6f8), 0);
    lv_obj_set_style_text_align(vert, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(vert, 0, 0);
    lv_label_set_text(vert, "y\ne\ns\nt\ne\nr\nd\na\ny");
    lv_obj_align(vert, LV_ALIGN_LEFT_MID, 6, 0);

    // Large calorie value, offset right of the vertical label
    s_value = lv_label_create(summary);
    lv_obj_set_style_text_font(s_value, ui_font_stat(), 0);
    lv_obj_set_style_text_color(s_value, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_value, "0");
    lv_obj_align(s_value, LV_ALIGN_CENTER, 12, 0);

    s_title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_title, ui_font_label(), 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xfc4c02), 0);
    lv_label_set_text(s_title, title ? title : "Calories Trend");
    lv_obj_align(s_title, LV_ALIGN_BOTTOM_LEFT, 3, -5);

    return s_screen;
}

void display_calorie_trend_update(const StravaData &data) {
    if (!s_screen || !s_value || !s_spark_line) return;

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
        s_pts[i].x = px;
        s_pts[i].y = py;
        lv_obj_set_pos(s_dots[i], px - kDotD / 2, py - kDotD / 2);
        lv_label_set_text(s_day_labels[i],
                          weekday_short(weekday_sun0(ref - (kPtCount - 1 - i))));
    }
    lv_line_set_points(s_spark_line, s_pts, kPtCount);
    lv_label_set_text_fmt(s_value, "%d", (int)lroundf(vals[kPtCount - 1]));
}
