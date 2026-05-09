#include "display_stats.h"
#include "ui_fonts.h"
#include <Arduino.h>

static lv_obj_t *s_screen        = nullptr;
static lv_obj_t *s_month_val     = nullptr;
static lv_obj_t *s_month_label   = nullptr; // updated with month name on data refresh
static lv_obj_t *s_total_val     = nullptr;
static lv_obj_t *s_busiest_val   = nullptr;
static lv_obj_t *s_age_label     = nullptr;

lv_obj_t *display_stats_build(const char *title) {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    const lv_color_t label_color = lv_color_white();
    const int screen_w  = 320;
    const int card_w    = 92;
    const int card_gap  = 10;
    const int cards_left = (screen_w - (card_w * 3 + card_gap * 2)) / 2;
    const int value_y   = 50;

    // Title label (Strava orange)
    lv_obj_t *title_lbl = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title_lbl, ui_font_label(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xfc4c02), 0);
    lv_obj_set_style_text_align(title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title_lbl, screen_w - 24);
    lv_label_set_text(title_lbl, title ? title : "Exercise Load");
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 10);

    // Three stat cards:
    //   [0] This month load (hours)   green
    //   [1] Total activities          blue
    //   [2] Busiest day (DD/MM)       amber
    struct CardDef {
        lv_obj_t **val_ptr;
        const char *static_label;
        lv_color_t  color;
    };
    CardDef cards[3] = {
        { &s_month_val,   "---\nload",    lv_color_hex(0x27ae60) },
        { &s_total_val,   "activities",  lv_color_hex(0x3498db) },
        { &s_busiest_val, "busiest\nday", lv_color_hex(0xe67e22) },
    };

    for (int i = 0; i < 3; i++) {
        int card_x = cards_left + i * (card_w + card_gap);

        *cards[i].val_ptr = lv_label_create(s_screen);
        lv_obj_set_style_text_font(*cards[i].val_ptr, ui_font_stat(), 0);
        lv_obj_set_style_text_color(*cards[i].val_ptr, cards[i].color, 0);
        lv_obj_set_style_text_align(*cards[i].val_ptr, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(*cards[i].val_ptr, card_w);
        lv_label_set_text(*cards[i].val_ptr, "--");
        lv_obj_set_pos(*cards[i].val_ptr, card_x, value_y);

        lv_obj_t *sub = lv_label_create(s_screen);
        lv_obj_set_style_text_font(sub, ui_font_label(), 0);
        lv_obj_set_style_text_color(sub, label_color, 0);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(sub, card_w);
        lv_label_set_text(sub, cards[i].static_label);
        lv_obj_align_to(sub, *cards[i].val_ptr, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
        if (i == 0) s_month_label = sub;
    }

    // Age footer
    s_age_label = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_age_label, ui_font_label(), 0);
    lv_obj_set_style_text_color(s_age_label, label_color, 0);
    lv_obj_set_style_text_align(s_age_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_age_label, screen_w - 24);
    lv_label_set_text(s_age_label, "");
    lv_obj_align(s_age_label, LV_ALIGN_BOTTOM_MID, 0, -10);

    return s_screen;
}

void display_stats_update(const StravaData &data) {
    if (!s_screen || !s_month_val || !s_total_val || !s_busiest_val) return;

    // Card 0: this month's load in hours (e.g. "12.5h")
    float month_hours = data.current_month_load / 60.0f;
    if (month_hours >= 10.0f)
        lv_label_set_text_fmt(s_month_val, "%.0fh", month_hours);
    else
        lv_label_set_text_fmt(s_month_val, "%.1fh", month_hours);

    // Update sub-label with month name
    if (s_month_label && data.current_month >= 1 && data.current_month <= 12) {
        static const char *MONTH_ABBRS[12] = {
            "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
            "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
        };
        lv_label_set_text_fmt(s_month_label, "%s\nload",
                              MONTH_ABBRS[data.current_month - 1]);
    }

    // Card 1: total active days
    lv_label_set_text_fmt(s_total_val, "%lu", (unsigned long)data.total_activities);

    // Card 2: busiest day (DD/MM)
    if (data.busiest_day_day > 0 && data.busiest_day_month > 0)
        lv_label_set_text_fmt(s_busiest_val, "%02u/%02u",
                              (unsigned)data.busiest_day_day,
                              (unsigned)data.busiest_day_month);
    else
        lv_label_set_text(s_busiest_val, "--/--");
}

void display_stats_set_age(uint32_t minutes_ago) {
    if (!s_age_label) return;
    if (minutes_ago == 0)
        lv_label_set_text(s_age_label, "just updated");
    else
        lv_label_set_text_fmt(s_age_label, "updated %dm ago", (int)minutes_ago);
}
