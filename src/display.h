#pragma once
#include <TFT_eSPI.h>
#include <Arduino.h>

// Screen dimensions in landscape orientation (setRotation(1) or (3))
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 170;

// RGB565 compile-time colour macro
#define RGB565(r, g, b) \
    ((uint16_t)(((uint16_t)((r) & 0xF8u) << 8) | ((uint16_t)((g) & 0xFCu) << 3) | ((uint8_t)(b) >> 3)))

// ── Shared colour palette ─────────────────────────────────────────────────────
static constexpr uint16_t C_BG       = 0x0000u;
static constexpr uint16_t C_WHITE    = 0xFFFFu;
static constexpr uint16_t C_STRAVA   = RGB565(0xFC, 0x4C, 0x02);
static constexpr uint16_t C_CYAN     = RGB565(0x4C, 0xC9, 0xF0);
static constexpr uint16_t C_AMBER    = RGB565(0xFF, 0xB4, 0x54);
static constexpr uint16_t C_RED_ERR  = RGB565(0xFF, 0x8F, 0x70);
static constexpr uint16_t C_GREEN    = RGB565(0x39, 0xD3, 0x53);
static constexpr uint16_t C_GREY_TXT = RGB565(0x8B, 0xA0, 0xB2);
static constexpr uint16_t C_PANEL    = RGB565(0x0D, 0x13, 0x1A);
static constexpr uint16_t C_BORDER   = RGB565(0x22, 0x30, 0x41);

// ── Activity level colours (load & calorie grids) ─────────────────────────────
static constexpr uint16_t LEVEL_BASE[5] = {
    RGB565(0x17, 0x1B, 0x20),  // 0 grey     - none
    RGB565(0x27, 0xAE, 0x60),  // 1 green    - low
    RGB565(0xF1, 0xC4, 0x0F),  // 2 yellow   - medium
    RGB565(0xE6, 0x7E, 0x22),  // 3 orange   - high
    RGB565(0xE7, 0x4C, 0x3C),  // 4 red      - very high
};
static constexpr uint16_t LEVEL_BRIGHT[5] = {
    RGB565(0x17, 0x1B, 0x20),  // 0 no animation
    RGB565(0x2E, 0xCC, 0x71),  // 1 bright green
    RGB565(0xFF, 0xD6, 0x3B),  // 2 bright yellow
    RGB565(0xF0, 0x93, 0x2B),  // 3 bright orange
    RGB565(0xFF, 0x52, 0x52),  // 4 bright red
};

// ── Global TFT and full-screen sprite ─────────────────────────────────────────
extern TFT_eSPI    g_tft;
extern TFT_eSprite g_sprite;

// Initialise display, create full-screen sprite.  Must be called in setup().
void    display_init(bool flip_screen);

// Push sprite to the physical display.
void    display_push();

// Linear interpolate between two RGB565 colours. t is 0.0-1.0.
uint16_t lerp_color(uint16_t base, uint16_t bright, float t);

// Draw multi-line text by splitting on '\n'.
// Uses TL_DATUM.  font is a TFT_eSPI built-in font number (1-8).
void draw_multiline(TFT_eSprite &spr, const char *text,
                    int x, int y, uint16_t color, int font, int line_h);
