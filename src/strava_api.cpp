#include "strava_api.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ── Level thresholds ──────────────────────────────────────────────────────────

uint8_t exercise_load_level(float load) {
    if (load <= 0.0f)   return 0;
    if (load < 50.0f)   return 1;
    if (load < 100.0f)  return 2;
    if (load < 150.0f)  return 3;
    return 4;
}

// ── Date helpers ──────────────────────────────────────────────────────────────

static int32_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= (int)(month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2u ? (unsigned)-3 : 9u)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int)doe - 719468;
}

static uint8_t weekday_sun0(int32_t d) {
    int w = (int)((d + 4) % 7);
    if (w < 0) w += 7;
    return (uint8_t)w;
}

static bool parse_iso_date(const char *s, int32_t &out_days, uint8_t &out_month, uint8_t &out_dom) {
    if (!s || strlen(s) < 10 || s[4] != '-' || s[7] != '-') return false;
    int y  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int mo = (s[5]-'0')*10  + (s[6]-'0');
    int d  = (s[8]-'0')*10  + (s[9]-'0');
    if (y <= 0 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    out_month = (uint8_t)mo;
    out_dom   = (uint8_t)d;
    out_days  = days_from_civil(y, (unsigned)mo, (unsigned)d);
    return true;
}

// ── Receive buffer ────────────────────────────────────────────────────────────
// Compact format: active days only as [offset,load] pairs — ~2-3 KB typical.
// Static to avoid heap fragmentation across fetches.
// NON-REENTRANT: strava_fetch() must never be called concurrently. The state
// machine in main.cpp (FETCH_RUNNING guard) enforces this; do not bypass it.

static const int BODY_BUF_SIZE = 20480;
static char      s_body[BODY_BUF_SIZE];
static bool      s_fetch_in_use = false;

class BufStream : public Stream {
public:
    int  _len      = 0;
    bool _overflow = false;
    size_t write(uint8_t c) override {
        if (_len >= BODY_BUF_SIZE - 1) { _overflow = true; return 1; }
        s_body[_len++] = (char)c;
        return 1;
    }
    size_t write(const uint8_t *buf, size_t sz) override {
        for (size_t i = 0; i < sz; i++) write(buf[i]);
        return sz;
    }
    int available() override { return 0; }
    int read()      override { return -1; }
    int peek()      override { return -1; }
};

// ── Zero-copy buffer helpers ──────────────────────────────────────────────────

static bool buf_is_json_ws(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void buf_skip_json_ws(const char *body, int len, int &pos) {
    while (pos < len && buf_is_json_ws(body[pos])) pos++;
}

// Find a JSON object key, tolerate whitespace around the colon, and leave pos
// at the first non-whitespace character of the value.
static bool buf_find_json_key(const char *body, int len, const char *key, int &pos) {
    int klen = (int)strlen(key);
    for (; pos <= len - klen; pos++) {
        if (memcmp(body + pos, key, klen) != 0) continue;

        int scan = pos + klen;
        buf_skip_json_ws(body, len, scan);
        if (scan >= len || body[scan] != ':') continue;

        scan++;
        buf_skip_json_ws(body, len, scan);
        pos = scan;
        return true;
    }
    return false;
}

static int buf_find_char(const char *body, int len, char ch, int start) {
    for (int i = start; i < len; i++) if (body[i] == ch) return i;
    return -1;
}

// Parse a float starting at pos; advance pos past the number. Returns 0 on fail.
static float buf_parse_float(const char *body, int len, int &pos) {
    buf_skip_json_ws(body, len, pos);
    if (pos >= len) return 0.0f;
    bool neg = (body[pos] == '-');
    if (neg) pos++;
    float v = 0.0f;
    while (pos < len && isDigit(body[pos])) v = v * 10.0f + (body[pos++] - '0');
    if (pos < len && body[pos] == '.') {
        pos++;
        float frac = 0.1f;
        while (pos < len && isDigit(body[pos])) {
            v += (body[pos++] - '0') * frac;
            frac *= 0.1f;
        }
    }
    return neg ? -v : v;
}

// Parse an unsigned integer starting at pos; advance pos past the number.
static int32_t buf_parse_int(const char *body, int len, int &pos) {
    buf_skip_json_ws(body, len, pos);
    int32_t v = 0;
    while (pos < len && isDigit(body[pos])) v = v * 10 + (body[pos++] - '0');
    return v;
}

// Parse a top-level float field like "total_load":12345.0
static float buf_parse_top_float(const char *body, int len, const char *key) {
    int pos = 0;
    if (buf_find_json_key(body, len, key, pos)) return buf_parse_float(body, len, pos);
    return 0.0f;
}

// ── Main fetch ────────────────────────────────────────────────────────────────

bool strava_fetch(const char *url, StravaData &data) {
    if (s_fetch_in_use) {
        Serial.println("[Strava] BUG: strava_fetch re-entered — aborting");
        return false;
    }
    s_fetch_in_use = true;

    data.valid               = false;
    data.week_count          = 0;
    data.total_load          = 0.0f;
    data.total_activities    = 0;
    data.busiest_day_load    = 0.0f;
    data.busiest_day_day     = 0;
    data.busiest_day_month   = 0;
    data.current_month_load  = 0;
    data.current_month       = 0;
    data.anchor_week_start_days = 0;
    data.latest_data_day_days   = 0;
    memset(data.days, 0, sizeof(data.days));

#define FETCH_FAIL(msg) do { Serial.println(msg); s_fetch_in_use = false; return false; } while(0)

    if (!url || url[0] == '\0')         FETCH_FAIL("[Strava] Missing server URL");
    if (WiFi.status() != WL_CONNECTED)  FETCH_FAIL("[Strava] WiFi not connected");

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    if (!http.begin(url))               FETCH_FAIL("[Strava] http.begin() failed");
    http.addHeader("Accept", "application/json");
    http.addHeader("Connection", "close");
    http.addHeader("User-Agent", "ESP32-T-DisplayS3-Strava");

    int code = http.GET();
    if (code <= 0) {
        Serial.printf("[Strava] HTTP error %d (%s) from %s\n", code, http.errorToString(code).c_str(), url);
        http.end();
        s_fetch_in_use = false;
        return false;
    }
    if (code != 200) {
        Serial.printf("[Strava] HTTP %d from %s\n", code, url);
        http.end();
        s_fetch_in_use = false;
        return false;
    }

    int content_len = http.getSize();
    // content_len == -1 means chunked/unknown — skip the pre-flight check but
    // still rely on BufStream overflow detection after the transfer.
    if (content_len != -1 && content_len > BODY_BUF_SIZE - 1) {
        Serial.printf("[Strava] Content-Length %d exceeds buffer %d\n", content_len, BODY_BUF_SIZE);
        http.end();
        s_fetch_in_use = false;
        return false;
    }
    if (content_len == -1) {
        Serial.println("[Strava] No Content-Length — overflow detection via BufStream");
    }

    BufStream buf_stream;
    http.writeToStream(&buf_stream);
    http.end();

    if (buf_stream._overflow) {
        Serial.printf("[Strava] Response overflow (>%d bytes)\n", BODY_BUF_SIZE);
        s_fetch_in_use = false;
        return false;
    }
    s_body[buf_stream._len] = '\0';
    int body_len = buf_stream._len;
    Serial.printf("[Strava] Response: %d bytes\n", body_len);

    if (body_len < 10) FETCH_FAIL("[Strava] Response too short");

    // ── Parse top-level stats ─────────────────────────────────────────────────
    data.total_load       = buf_parse_top_float(s_body, body_len, "\"total_load\"");
    data.total_activities = (uint32_t)buf_parse_top_float(s_body, body_len, "\"total_activities\"");
    float busiest_load    = buf_parse_top_float(s_body, body_len, "\"busiest_load\"");

    // Parse busiest_date: "YYYY-MM-DD"
    {
        static const char BD_KEY[] = "\"busiest_date\"";
        int bpos = 0;
        if (buf_find_json_key(s_body, body_len, BD_KEY, bpos) && bpos < body_len && s_body[bpos] == '"' && bpos + 11 <= body_len) {
            char bdate[11];
            memcpy(bdate, s_body + bpos + 1, 10);
            bdate[10] = '\0';
            int32_t bdays = 0;
            uint8_t bmonth = 0, bdom = 0;
            if (parse_iso_date(bdate, bdays, bmonth, bdom)) {
                data.busiest_day_load  = busiest_load;
                data.busiest_day_day   = bdom;
                data.busiest_day_month = bmonth;
            }
        }
    }

    // ── Parse "start" date ────────────────────────────────────────────────────
    // "start":"YYYY-MM-DD" is the epoch for offset 0 in the days array.
    int32_t start_epoch = 0;
    {
        static const char START_KEY[] = "\"start\"";
        int spos = 0;
        if (!buf_find_json_key(s_body, body_len, START_KEY, spos) ||
            spos >= body_len || s_body[spos] != '"' || spos + 11 > body_len) {
            Serial.println("[Strava] 'start' field not found");
            s_fetch_in_use = false;
            return false;
        }
        char start_str[11];
        memcpy(start_str, s_body + spos + 1, 10);
        start_str[10] = '\0';
        uint8_t sm = 0, sd = 0;
        if (!parse_iso_date(start_str, start_epoch, sm, sd)) {
            Serial.printf("[Strava] Bad start date: %s\n", start_str);
            s_fetch_in_use = false;
            return false;
        }
    }

    // ── Parse days array ─────────────────────────────────────────────────────
    // Format: "days":[[offset,load],...] — active days only, zeroes omitted.
    static const char DAYS_KEY[] = "\"days\"";
    int pos = 0;
    if (!buf_find_json_key(s_body, body_len, DAYS_KEY, pos) || pos >= body_len || s_body[pos] != '[') {
        Serial.println("[Strava] 'days' array not found");
        s_fetch_in_use = false;
        return false;
    }
    pos++;  // skip outer '['

    struct ParsedDay {
        int32_t days_epoch;
        float   load;
        uint8_t month;
        uint8_t dom;
    };
    static ParsedDay s_pd[GRID_WEEKS * GRID_DAYS];
    int pd_count = 0;

    int32_t latest_day = INT32_MIN;

    while (pd_count < GRID_WEEKS * GRID_DAYS) {
        buf_skip_json_ws(s_body, body_len, pos);
        if (pos >= body_len || s_body[pos] == ']') break;  // end of outer array
        if (s_body[pos] != '[') { pos++; continue; }       // skip commas/garbage
        pos++;  // skip inner '['

        int32_t offset   = buf_parse_int(s_body, body_len, pos);
        buf_skip_json_ws(s_body, body_len, pos);
        if (pos >= body_len || s_body[pos] != ',') continue;
        pos++;
        float load_val = buf_parse_float(s_body, body_len, pos);

        // advance past the closing ']' of this pair
        int pair_end = buf_find_char(s_body, body_len, ']', pos);
        if (pair_end < 0) break;
        pos = pair_end + 1;

        if (load_val <= 0.0f) continue;  // defensive; server already skips zeroes

        int32_t d_epoch = start_epoch + offset;

        // civil_from_days — inverse of days_from_civil
        {
            int32_t  z   = d_epoch + 719468;
            int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
            uint32_t doe = (uint32_t)(z - era * 146097);
            uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
            uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);
            uint32_t mp  = (5*doy + 2) / 153;
            uint8_t  dom = (uint8_t)(doy - (153*mp+2)/5 + 1);
            uint8_t  mo  = (uint8_t)(mp < 10 ? mp+3 : mp-9);

            s_pd[pd_count].days_epoch = d_epoch;
            s_pd[pd_count].load       = load_val;
            s_pd[pd_count].month      = mo;
            s_pd[pd_count].dom        = dom;
            pd_count++;

            if (d_epoch > latest_day) latest_day = d_epoch;
        }
    }

    if (pd_count == 0) {
        Serial.println("[Strava] No days parsed");
        s_fetch_in_use = false;
        return false;
    }

    data.latest_data_day_days   = latest_day;
    data.anchor_week_start_days = latest_day - weekday_sun0(latest_day);
    data.week_count             = GRID_WEEKS;

    // ── Map into grid ─────────────────────────────────────────────────────────
    for (int i = 0; i < pd_count; i++) {
        uint8_t  wd    = weekday_sun0(s_pd[i].days_epoch);
        int32_t  ws    = s_pd[i].days_epoch - wd;
        int32_t  delta = data.anchor_week_start_days - ws;
        if (delta < 0 || (delta % 7) != 0) continue;
        int week_diff = (int)(delta / 7);
        if (week_diff >= GRID_WEEKS) continue;
        int tw = (GRID_WEEKS - 1) - week_diff;               // 0 = oldest, 52 = current
        data.days[tw][wd].load  = s_pd[i].load;
        data.days[tw][wd].level = exercise_load_level(s_pd[i].load);
    }

    // ── Current-month load ────────────────────────────────────────────────────
    if (latest_day != INT32_MIN) {
        time_t secs = (time_t)latest_day * 86400;
        struct tm tm_now;
        gmtime_r(&secs, &tm_now);
        data.current_month = (uint8_t)(tm_now.tm_mon + 1);

        int cy = tm_now.tm_year + 1900;
        int nm = (data.current_month == 12) ? 1 : data.current_month + 1;
        int ny = (data.current_month == 12) ? cy + 1 : cy;
        int32_t mo_start  = days_from_civil(cy, data.current_month, 1);
        int32_t mo_end    = days_from_civil(ny, (unsigned)nm, 1);

        float mo_load = 0.0f;
        for (int i = 0; i < pd_count; i++) {
            if (s_pd[i].days_epoch >= mo_start && s_pd[i].days_epoch < mo_end)
                mo_load += s_pd[i].load;
        }
        data.current_month_load = (uint16_t)(mo_load + 0.5f);
    }

    data.valid = true;
    s_fetch_in_use = false;
    Serial.printf("[Strava] OK - %d active days, total=%.0f min, month=%d load=%d min, busiest=%.0f min\n",
        pd_count, data.total_load, data.current_month,
        data.current_month_load, data.busiest_day_load);
    return true;
}
