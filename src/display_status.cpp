#include "display_status.h"

void display_status_render(TFT_eSprite &spr, const char *title,
                           const char *body, uint16_t accent) {
    // ── Decorative orbs ────────────────────────────────────────────────────
    // Orb A: 120 px dia, top-left corner partially offscreen
    spr.fillCircle(24, 32, 60, RGB565(0x10, 0x31, 0x4D));
    // Orb B: 92 px dia, bottom-right corner partially offscreen
    spr.fillCircle(300, 132, 46, RGB565(0x30, 0x21, 0x0F));

    // ── Central panel: 304×148, centred on 320×170 ─────────────────────────
    const int px = 8, py = 11, pw = 304, ph = 148, pr = 18;
    spr.fillRoundRect(px, py, pw, ph, pr, C_PANEL);
    spr.drawRoundRect(px, py, pw, ph, pr, C_BORDER);

    // Panel content starts at (px+14, py+10)
    const int cx = px + 14;
    const int cy = py + 10;

    // Accent bar: 64×3 px
    spr.fillRect(cx, cy, 64, 3, accent);

    // Title: Font4 (~26 px tall), 8 px below accent bar
    spr.setTextFont(4);
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(TFT_WHITE);
    if (title && title[0]) spr.drawString(title, cx, cy + 6);

    // Body: Font2 (~16 px tall), 4 px below title
    const int body_y = cy + 6 + 28;
    draw_multiline(spr, body, cx, body_y, C_GREY_TXT, 2, 20);
}