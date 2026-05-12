#pragma once
#include "display.h"

// Render the status/loading screen directly into the provided sprite.
void display_status_render(TFT_eSprite &spr, const char *title,
                           const char *body, uint16_t accent);