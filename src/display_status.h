#pragma once

#include <lvgl.h>

lv_obj_t *display_status_build();
void display_status_update(const char *title, const char *body, lv_color_t accent);