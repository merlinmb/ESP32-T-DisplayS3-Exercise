#include "display.h"
#include "PINS_T_DISPLAY_S3.h"
#include <esp_heap_caps.h>
#include <string.h>

TFT_eSPI    g_tft;
TFT_eSprite g_sprite(&g_tft);

void display_init(bool flip_screen) {
    DEV_DEVICE_INIT();
    g_tft.init();
    g_tft.setRotation(flip_screen ? 3 : 1);
    g_tft.fillScreen(TFT_BLACK);

    g_sprite.setColorDepth(16);

    // Prefer PSRAM for the 320×170×2 = 108 800-byte sprite buffer.
    // heap_caps_malloc returns nullptr on failure; createSprite() then falls
    // back to MALLOC_CAP_DEFAULT (which also includes PSRAM as a last resort).
    size_t sprite_bytes = (size_t)DISP_W * DISP_H * 2u;
    void  *psram_buf    = heap_caps_malloc(sprite_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_buf) {
        // Hand the pre-allocated PSRAM buffer to the sprite.
        g_sprite.createSprite(DISP_W, DISP_H);
        // If createSprite chose a different buffer, keep the one TFT_eSPI
        // allocated internally; free our reservation and rely on the fallback.
        if (!g_sprite.getPointer()) {
            // Internal alloc also failed — sprite already owns nothing.
            // Map our PSRAM allocation as the frame buffer manually.
            // (TFT_eSPI ≥ 2.5: frameBuffer is available but not always exposed;
            //  use the simpler approach of always relying on createSprite.)
            free(psram_buf);
        } else {
            free(psram_buf);   // createSprite already allocated; drop reservation
        }
    } else {
        // No PSRAM available (rare) — fall back to any available heap.
        g_sprite.createSprite(DISP_W, DISP_H);
    }

    if (!g_sprite.getPointer()) {
        Serial.println("[Display] FATAL: sprite alloc failed");
        while (true) { delay(500); }
    }

    g_sprite.fillSprite(TFT_BLACK);
    Serial.printf("[Display] Sprite allocated %d×%d, ptr=%p\n",
                  DISP_W, DISP_H, g_sprite.getPointer());
}

void display_push() {
    g_sprite.pushSprite(0, 0);
}

uint16_t lerp_color(uint16_t base, uint16_t bright, float t) {
    if (t <= 0.0f) return base;
    if (t >= 1.0f) return bright;
    int r0 = (base   >> 11) & 0x1F;
    int g0 = (base   >>  5) & 0x3F;
    int b0 =  base          & 0x1F;
    int r1 = (bright >> 11) & 0x1F;
    int g1 = (bright >>  5) & 0x3F;
    int b1 =  bright        & 0x1F;
    int r  = r0 + (int)((float)(r1 - r0) * t);
    int g  = g0 + (int)((float)(g1 - g0) * t);
    int b  = b0 + (int)((float)(b1 - b0) * t);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void draw_multiline(TFT_eSprite &spr, const char *text,
                    int x, int y, uint16_t color, int font, int line_h) {
    if (!text) return;
    spr.setTextFont(font);
    spr.setTextColor(color);
    spr.setTextDatum(TL_DATUM);
    int cy = y;
    const char *p = text;
    char buf[128];
    while (*p) {
        const char *nl  = (const char *)memchr(p, '\n', 512);
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        if (len > 0) spr.drawString(buf, x, cy);
        cy += line_h;
        p  += len;
        if (*p == '\n') p++;
    }
}
