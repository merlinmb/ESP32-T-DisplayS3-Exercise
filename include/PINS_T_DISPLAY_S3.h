#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// LilyGo T-Display S3  (1.9" ST7789V  170 x 320, ESP32-S3)
// TFT_eSPI is configured via USER_SETUP_LOADED defines in platformio.ini (Setup206 / ST7789 8-bit parallel)
// ─────────────────────────────────────────────────────────────────────────────

#if !defined(ESP32)
#error "Please select an ESP32-S3 board in platformio.ini"
#endif

#define DEV_DEVICE_PINS

// The T-Display S3 needs its panel power-enable pin asserted before the
// display controller will respond on the bus.
#define DEV_DEVICE_INIT()                 \
    {                                     \
        pinMode(15 /* PWD */, OUTPUT);    \
        digitalWrite(15 /* PWD */, HIGH); \
    }

// ── Buttons ────────────────────────────────────────────────────────────────────
#define BTN_A 0    // BOOT / left button (active LOW)
#define BTN_B 14   // right side button  (active LOW)

// ── Backlight ─────────────────────────────────────────────────────────────────
// 16-step pulse-counting driver (NOT PWM). See apply_brightness() in main.cpp.
#define GFX_BL 38
