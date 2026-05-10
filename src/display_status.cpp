#include "display_status.h"
#include "ui_fonts.h"

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_accent = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_body = nullptr;

lv_obj_t *display_status_build() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *orb_a = lv_obj_create(s_screen);
    lv_obj_remove_style_all(orb_a);
    lv_obj_set_size(orb_a, 120, 120);
    lv_obj_set_style_radius(orb_a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb_a, lv_color_hex(0x10314d), 0);
    lv_obj_set_style_bg_opa(orb_a, LV_OPA_40, 0);
    lv_obj_set_pos(orb_a, -36, -28);

    lv_obj_t *orb_b = lv_obj_create(s_screen);
    lv_obj_remove_style_all(orb_b);
    lv_obj_set_size(orb_b, 92, 92);
    lv_obj_set_style_radius(orb_b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb_b, lv_color_hex(0x30210f), 0);
    lv_obj_set_style_bg_opa(orb_b, LV_OPA_30, 0);
    lv_obj_set_pos(orb_b, 254, 86);

    lv_obj_t *panel = lv_obj_create(s_screen);
    lv_obj_set_size(panel, 304, 148);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0d131a), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x223041), 0);
    lv_obj_set_style_pad_hor(panel, 14, 0);
    lv_obj_set_style_pad_ver(panel, 10, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    s_accent = lv_obj_create(panel);
    lv_obj_remove_style_all(s_accent);
    lv_obj_set_size(s_accent, 64, 3);
    lv_obj_set_style_radius(s_accent, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_accent, lv_color_hex(0x4cc9f0), 0);
    lv_obj_set_style_bg_opa(s_accent, LV_OPA_COVER, 0);
    lv_obj_align(s_accent, LV_ALIGN_TOP_LEFT, 0, 0);

    s_title = lv_label_create(panel);
    lv_obj_set_width(s_title, 276);
    lv_obj_set_style_text_font(s_title, ui_font_label(), 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(s_title, 1, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_title, "Loading activity");
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 0, 8);

    s_body = lv_label_create(panel);
    lv_obj_set_width(s_body, 276);
    lv_obj_set_style_text_font(s_body, ui_font_label(), 0);
    lv_obj_set_style_text_color(s_body, lv_color_hex(0x8ba0b2), 0);
    lv_obj_set_style_text_line_space(s_body, 2, 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_body, "Waiting for the first Strava payload.");
    lv_obj_align(s_body, LV_ALIGN_TOP_LEFT, 0, 30);

    return s_screen;
}

void display_status_update(const char *title, const char *body, lv_color_t accent) {
    if (!s_screen || !s_title || !s_body || !s_accent) return;

    lv_label_set_text(s_title, (title && title[0] != '\0') ? title : "Status");
    lv_label_set_text(s_body, (body && body[0] != '\0') ? body : "Waiting for the first Strava payload.");
    lv_obj_set_style_bg_color(s_accent, accent, 0);
}