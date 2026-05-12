#include "display_stats.h"
#include <Arduino.h>
#include <stdio.h>
// FreeSansBold18pt7b & FreeSansBold9pt7b are already included via TFT_eSPI.h -> gfxfont.h

void display_stats_render(TFT_eSprite &spr, const StravaData &data,
                          uint32_t age_minutes) {
    // Three stat cards side by side, vertically centred.
    // Layout: screen 320×170, footer 28 px, cards 92×90 px, 10 px gap.
    static const int FOOTER_H = 28;
    static const int CARD_W   = 92;
    static const int CARD_H   = 110;
    static const int CARD_GAP = 10;
    static const int CARDS_LEFT = (320 - (CARD_W * 3 + CARD_GAP * 2)) / 2;  // 17
    static const int CARD_TOP   = (170 - FOOTER_H - CARD_H) / 2;            // 26

    struct CardDef {
        const char *label;
        uint16_t    color;
    };
    static const CardDef CARDS[3] = {
        { "load",       RGB565(0x27, 0xAE, 0x60) },  // green
        { "activities", RGB565(0x34, 0x98, 0xDB) },  // blue
        { "peak date",  RGB565(0xE6, 0x7E, 0x22) },  // orange
    };

    // ── Draw three cards ─────────────────────────────────────────────────────
    for (int i = 0; i < 3; i++) {
        int cx = CARDS_LEFT + i * (CARD_W + CARD_GAP);

        // Card background
        spr.fillRoundRect(cx, CARD_TOP, CARD_W, CARD_H, 10, C_PANEL);
        spr.drawRoundRect(cx, CARD_TOP, CARD_W, CARD_H, 10, C_BORDER);

        // Value label (Font4, ~26 px) – centred horizontally
        char val_buf[16] = "--";
        if (data.valid) {
            if (i == 0) {
                float hours = (float)data.current_month_load / 60.0f;
                if (hours >= 10.0f) snprintf(val_buf, sizeof(val_buf), "%.0fh", hours);
                else                snprintf(val_buf, sizeof(val_buf), "%.1fh", hours);
            } else if (i == 1) {
                snprintf(val_buf, sizeof(val_buf), "%lu", (unsigned long)data.total_activities);
            } else {
                if (data.busiest_day_day > 0 && data.busiest_day_month > 0)
                    snprintf(val_buf, sizeof(val_buf), "%02u/%02u",
                             (unsigned)data.busiest_day_day,
                             (unsigned)data.busiest_day_month);
            }
        }
        spr.setFreeFont(&FreeSansBold18pt7b);
        spr.setTextSize(1);
        spr.setTextColor(CARDS[i].color);
        spr.setTextDatum(TC_DATUM);
        spr.drawString(val_buf, cx + CARD_W / 2, CARD_TOP + 8);

        // Sub-label (FreeSansBold9pt7b, bold) – centred in card
        char sub_buf[16];
        if (i == 0 && data.valid && data.current_month >= 1 && data.current_month <= 12) {
            static const char *MO[12] = {
                "JAN","FEB","MAR","APR","MAY","JUN",
                "JUL","AUG","SEP","OCT","NOV","DEC"
            };
            snprintf(sub_buf, sizeof(sub_buf), "%s load", MO[data.current_month - 1]);
        } else {
            strncpy(sub_buf, CARDS[i].label, sizeof(sub_buf) - 1);
            sub_buf[sizeof(sub_buf) - 1] = '\0';
        }
        spr.setFreeFont(&FreeSansBold9pt7b);
        spr.setTextSize(1);
        spr.setTextColor(TFT_WHITE);
        spr.setTextDatum(TC_DATUM);
        spr.drawString(sub_buf, cx + CARD_W / 2, CARD_TOP + 77);
        spr.setTextFont(2);  // restore built-in font
    }

    // ── Footer ────────────────────────────────────────────────────────────────
    const int fy = 170 - FOOTER_H;
    spr.fillRect(0, fy, 320, FOOTER_H, TFT_BLACK);

    spr.setTextFont(2);
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(C_STRAVA);
    spr.drawString("Exercise Load", 320 / 2, fy + FOOTER_H / 2);

    // Age label (bottom-right)
    char age_buf[32] = "";
    if (age_minutes == 0)
        strncpy(age_buf, "just updated", sizeof(age_buf) - 1);
    else
        snprintf(age_buf, sizeof(age_buf), "updated %dm ago", (int)age_minutes);

    spr.setTextDatum(MR_DATUM);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(age_buf, 317, fy + FOOTER_H / 2);
}

