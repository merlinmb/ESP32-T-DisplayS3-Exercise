#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// LilyGo T-Display S3  (1.9" ST7789V  170 x 320, ESP32-S3)
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

// ── Display ───────────────────────────────────────────────────────────────────
#define GFX_SUPPORTED
#include <Arduino_GFX_Library.h>

// Backlight pin
#define GFX_BL 38

// The regular T-Display S3 uses an 8080-style 8-bit parallel LCD bus.
Arduino_DataBus *bus = new Arduino_ESP32LCD8(
    7  /* DC */,
    6  /* CS */,
    8  /* WR */,
    9  /* RD */,
    39 /* D0 */,
    40 /* D1 */,
    41 /* D2 */,
    42 /* D3 */,
    45 /* D4 */,
    46 /* D5 */,
    47 /* D6 */,
    48 /* D7 */
);

// ST7789V  170 wide x 320 tall (portrait).  Rotation applied in main.cpp.
// col offset 35 centres the 170-px viewport inside the 240-column controller.
Arduino_ST7789 *gfx = new Arduino_ST7789(
    bus,
    5    /* RST */,
    0    /* rotation — overridden by setRotation() in main.cpp */,
    true /* IPS */,
    170  /* width  */,
    320  /* height */,
    35   /* col offset 1 */,
    0    /* row offset 1 */,
    35   /* col offset 2 */,
    0    /* row offset 2 */
);

#define GFX_SPEED 80000000UL

// ── NeoPixel ──────────────────────────────────────────────────────────────────
// The standard T-Display S3 has NO built-in NeoPixel.
// Uncomment if you have soldered an external WS2812 to GPIO 21:
// #define NEOPIXEL_SUPPORTED
// #define NEOPIXEL_PIN 21
// #define NEOPIXEL_DEFAULT_BRIGHTNESS 4
// #define NEOPIXEL_WIDTH  1
// #define NEOPIXEL_HEIGHT 1
