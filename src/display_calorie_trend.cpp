#include "display_calorie_trend.h"
#include <Arduino.h>
#include <math.h>

// ── Layout constants ──────────────────────────────────────────────────────────
// Screen 320×170.  Chart card fills from left (1px pad) to just before summary.
// Summary card flush to the right edge (1px pad).
static const int kSummaryW   =  84;
static const int kSummaryH   = 137;
static const int kSummaryX   = DISP_W - kSummaryW - 1;  // 235
static const int kSummaryY   =   4;

static const int kChartX     =   1;
static const int kChartY     =   4;   // top-aligned with summary card
static const int kChartW     = kSummaryX - kChartX - 4;  // 230 (4px gap)
static const int kChartH     = 137;

static const int kPtCount    =  7;
// Spark content inside chart card: pushed right & down to clear rounded corners
static const int kSparkLeft  = 15;   // was 7
static const int kSparkRight = 210;  // was 189
static const int kSparkTop   = 10;   // was 4
static const int kSparkBot   = 100;  // was 94  (+6 to match top shift)
static const int kBaseline   = 105;  // was 99  (+6 to match top shift)
static const int kDotD       =  7;
static const int kTodayDotD  = 11;
static const int kTodayHaloD = 15;  // kTodayDotD + 4
static const int kCurveSamplesPerSegment = 10;
static const int kCurvePtCount = (kPtCount - 1) * kCurveSamplesPerSegment + 1;

// ── Point struct (replaces lv_point_precise_t) ───────────────────────────────
struct Pt { float x; float y; };

// ── Calorie level colour ──────────────────────────────────────────────────────
static uint16_t cal_level_color(uint8_t level) {
    static const uint16_t kColors[5] = {
        RGB565(0x17, 0x1B, 0x20),   // 0 grey
        RGB565(0x27, 0xAE, 0x60),   // 1 green
        RGB565(0xF1, 0xC4, 0x0F),   // 2 yellow
        RGB565(0xE6, 0x7E, 0x22),   // 3 orange
        RGB565(0xE7, 0x4C, 0x3C),   // 4 red
    };
    if (level > 4) level = 4;
    return kColors[level];
}

// ── Calendar helpers ──────────────────────────────────────────────────────────
static uint8_t weekday_sun0(int32_t d) {
    int w = (int)((d + 4) % 7);
    if (w < 0) w += 7;
    return (uint8_t)w;
}

static const char *weekday_short(uint8_t d) {
    static const char *kL[7] = {"S","M","T","W","T","F","S"};
    return kL[(d < 7) ? d : 0];
}

static bool day_calories(const StravaData &data, int32_t day_epoch, float &cal) {
    if (data.anchor_week_start_days == 0) return false;
    uint8_t wd    = weekday_sun0(day_epoch);
    int32_t ws    = day_epoch - (int32_t)wd;
    int32_t delta = data.anchor_week_start_days - ws;
    if (delta < 0 || (delta % 7) != 0) return false;
    int wk = (int)(delta / 7);
    if (wk >= GRID_WEEKS) return false;
    cal = data.days[(GRID_WEEKS - 1) - wk][wd].calories;
    return true;
}

// ── Sparkline geometry helpers ────────────────────────────────────────────────
static int pt_x(int i) {
    return kSparkLeft + (i * (kSparkRight - kSparkLeft)) / (kPtCount - 1);
}

static int pt_y(float norm) {
    return kSparkTop + (int)lroundf((1.0f - norm) * (float)(kSparkBot - kSparkTop));
}

// ── Catmull-Rom spline ────────────────────────────────────────────────────────
static Pt s_pts[kPtCount];
static Pt s_curve_pts[kCurvePtCount];

static void rebuild_smooth_curve_points() {
    int out_idx = 0;
    for (int i = 0; i < (kPtCount - 1); ++i) {
        const Pt p0 = s_pts[(i == 0) ? 0 : (i - 1)];
        const Pt p1 = s_pts[i];
        const Pt p2 = s_pts[i + 1];
        const Pt p3 = s_pts[(i + 2 < kPtCount) ? (i + 2) : (kPtCount - 1)];

        const float cp1x = p1.x + (p2.x - p0.x) / 6.0f;
        const float cp1y = p1.y + (p2.y - p0.y) / 6.0f;
        const float cp2x = p2.x - (p3.x - p1.x) / 6.0f;
        const float cp2y = p2.y - (p3.y - p1.y) / 6.0f;

        for (int s = 0; s < kCurveSamplesPerSegment; ++s) {
            const float t = (float)s / (float)kCurveSamplesPerSegment;
            const float u = 1.0f - t;
            s_curve_pts[out_idx].x = u*u*u*p1.x + 3.0f*u*u*t*cp1x +
                                     3.0f*u*t*t*cp2x + t*t*t*p2.x;
            s_curve_pts[out_idx].y = u*u*u*p1.y + 3.0f*u*u*t*cp1y +
                                     3.0f*u*t*t*cp2y + t*t*t*p2.y;
            out_idx++;
        }
    }
    s_curve_pts[out_idx] = s_pts[kPtCount - 1];
}

// ── Delta marker helpers ──────────────────────────────────────────────────────
// Draw a simple 3-row filled triangle or flat bar in the summary card.
// wx,wy = top-left corner of the 16×10 marker area within the sprite.
static void draw_delta_marker(TFT_eSprite &spr, int wx, int wy, float delta) {
    if (delta > 0.5f) {
        // Up triangle: rows narrow from bottom to top
        static const int up_w[3] = {13, 9, 5};
        uint16_t col = RGB565(0x32, 0xCD, 0x32);
        for (int r = 0; r < 3; r++)
            spr.fillRect(wx + (16 - up_w[r]) / 2, wy + (2 - r) * 3, up_w[r], 2, col);
    } else if (delta < -0.5f) {
        // Down triangle: rows narrow from top to bottom
        static const int dn_w[3] = {13, 9, 5};
        uint16_t col = RGB565(0xFF, 0x4D, 0x4F);
        for (int r = 0; r < 3; r++)
            spr.fillRect(wx + (16 - dn_w[r]) / 2, wy + r * 3, dn_w[r], 2, col);
    } else {
        // Flat bar
        spr.fillRect(wx + 2, wy + 4, 12, 2, RGB565(0xB0, 0xBE, 0xC5));
    }
}

// ── Main render ───────────────────────────────────────────────────────────────
void display_calorie_trend_render(TFT_eSprite &spr, const StravaData &data) {
    // ── Chart card ────────────────────────────────────────────────────────────
    spr.fillRoundRect(kChartX, kChartY, kChartW, kChartH, 22,
                      RGB565(0x0B, 0x13, 0x1A));
    spr.drawRoundRect(kChartX, kChartY, kChartW, kChartH, 22,
                      RGB565(0x1F, 0x2D, 0x39));

    // Guide lines (3 horizontal)
    const int kSpan = kSparkRight - kSparkLeft;
    for (int i = 0; i < 3; i++) {
        int gy = kSparkTop + i * (kSparkBot - kSparkTop) / 2;
        spr.drawFastHLine(kChartX + kSparkLeft, kChartY + gy,
                          kSpan, RGB565(0x17, 0x30, 0x42));
    }
    // Baseline
    spr.fillRect(kChartX + kSparkLeft, kChartY + kBaseline,
                 kSpan, 2, RGB565(0x2D, 0x46, 0x58));

    // ── Build sparkline data ──────────────────────────────────────────────────
    int32_t ref = 0;
    if (data.current_day_days != 0)
        ref = data.current_day_days - 1;
    else if (data.latest_data_day_days != 0)
        ref = data.latest_data_day_days;

    float vals[kPtCount] = {};
    float mx = 0.0f;
    for (int i = 0; i < kPtCount; i++) {
        float cal = 0.0f;
        if (ref != 0 && data.valid)
            day_calories(data, ref - (kPtCount - 1 - i), cal);
        vals[i] = cal;
        if (cal > mx) mx = cal;
    }
    if (mx < 1.0f) mx = 1.0f;

    for (int i = 0; i < kPtCount; i++) {
        s_pts[i].x = (float)pt_x(i);
        s_pts[i].y = (float)pt_y(vals[i] / mx);
    }
    rebuild_smooth_curve_points();

    // ── Draw sparkline ────────────────────────────────────────────────────────
    uint16_t trend_col = RGB565(0x4C, 0xC9, 0xF0);
    for (int i = 0; i < kCurvePtCount - 1; i++) {
        spr.drawLine(
            kChartX + (int)lroundf(s_curve_pts[i].x),
            kChartY + (int)lroundf(s_curve_pts[i].y),
            kChartX + (int)lroundf(s_curve_pts[i + 1].x),
            kChartY + (int)lroundf(s_curve_pts[i + 1].y),
            trend_col);
    }

    // ── Dots + day labels ─────────────────────────────────────────────────────
    for (int i = 0; i < kPtCount; i++) {
        int px = kChartX + pt_x(i);
        int py = kChartY + pt_y(vals[i] / mx);
        const int dot_d = (i == kPtCount - 1) ? kTodayDotD : kDotD;
        uint8_t lvl = data.valid ? calorie_burn_level(vals[i]) : 0u;
        uint16_t dcol = cal_level_color(lvl);

        // Today halo (ring)
        if (i == kPtCount - 1) {
            spr.drawCircle(px, py, kTodayHaloD / 2, dcol);
        }
        spr.fillCircle(px, py, dot_d / 2, dcol);

        // Day label
        const char *dl = (ref != 0)
            ? weekday_short(weekday_sun0(ref - (kPtCount - 1 - i)))
            : "-";
        spr.setTextFont(2);
        spr.setTextColor(RGB565(0x9C, 0xB0, 0xC1));
        spr.setTextDatum(TC_DATUM);
        spr.drawString(dl, px, kChartY + kBaseline + 6);
    }

    // ── Summary card ─────────────────────────────────────────────────────────
    spr.fillRoundRect(kSummaryX, kSummaryY, kSummaryW, kSummaryH, 20,
                      RGB565(0x11, 0x1A, 0x22));
    spr.drawRoundRect(kSummaryX, kSummaryY, kSummaryW, kSummaryH, 20,
                      RGB565(0x26, 0x37, 0x46));

    // Header "day - 1"
    spr.setTextFont(2);
    spr.setTextColor(RGB565(0x7F, 0xD6, 0xF8));
    spr.setTextDatum(TC_DATUM);
    spr.drawString("day - 1", kSummaryX + kSummaryW / 2, kSummaryY + 6);

    // Large calorie value
    float day_minus_1 = vals[kPtCount - 1];
    float day_minus_2 = vals[kPtCount - 2];
    char val_buf[8];
    snprintf(val_buf, sizeof(val_buf), "%d", (int)lroundf(day_minus_1));
    spr.setTextFont(4);
    spr.setTextColor(TFT_WHITE);
    spr.setTextDatum(MC_DATUM);
    spr.drawString(val_buf, kSummaryX + kSummaryW / 2, kSummaryY + kSummaryH / 2 - 12);

    // Delta marker
    draw_delta_marker(spr,
        kSummaryX + (kSummaryW - 16) / 2,
        kSummaryY + kSummaryH / 2 + 16,
        day_minus_1 - day_minus_2);

    // "k.cal" unit label (bottom, aligned to bottom of day labels in chart)
    const int unit_y = kChartY + kBaseline + 6 - kSummaryY;
    spr.setTextFont(2);
    spr.setTextColor(RGB565(0x9C, 0xB0, 0xC1));
    spr.setTextDatum(TC_DATUM);
    spr.drawString("k.cal", kSummaryX + kSummaryW / 2, kSummaryY + unit_y);

    // Title bottom-centre
    spr.setTextFont(2);
    spr.setTextDatum(BC_DATUM);
    spr.setTextColor(C_STRAVA);
    spr.drawString("Calories Trend", DISP_W / 2, DISP_H - 5);
}

