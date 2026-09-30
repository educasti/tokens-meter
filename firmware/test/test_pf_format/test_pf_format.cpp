// Host check for the portfolio number formatters (portfolio spec §7). The
// functions mirror src/ui_portfolio.cpp so the formats can be verified without
// a board. The mirror is by hand, not generated: fmt_sym is the one that drifts,
// because the firmware measures a string in px and compares it against a budget
// the row layout computed, while this file stands in a glyph width table for
// LVGL. If you change a formatter in the firmware, change it here too.
// Build & run:
//   (cd firmware/test/test_pf_format && g++ -std=c++17 test_pf_format.cpp -o /tmp/pf && /tmp/pf)
#include <stdio.h>
#include <string.h>
#include <string>

// ---- shape copied from src/ui_portfolio.cpp; fmt_sym mirrors it against a
// px budget rather than a character count, per the row layout (spec §2.3.1) --

static const char *pf_sign(long n) {
    return n > 0 ? "+" : n < 0 ? "-" : "";
}

static void fmt_int(char *buf, size_t len, long v) {
    char tmp[16];
    int n = 0;
    unsigned long a = (unsigned long)(v < 0 ? -v : v);
    do { tmp[n++] = (char)('0' + (a % 10)); a /= 10; } while (a);
    size_t o = 0;
    for (int i = n - 1; i >= 0 && o + 1 < len; i--) {
        // Groups run right to left, so a comma starts a new group wherever a
        // whole group of three is already complete: `i` is the count of digits
        // written so far, and the leftmost digit (i == n-1) never gets one.
        // 24500 -> "24,500", 122900 -> "122,900", 375 -> "375".
        if (i < n - 1 && ((i + 1) % 3) == 0 && o + 1 < len) buf[o++] = ',';
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
}

static const char *fmt_market_value(long mv) {
    static char buf[16];
    long a = mv < 0 ? -mv : mv;
    if (a < 1000000L) {
        fmt_int(buf, sizeof buf, mv);
    } else if (a < 999950000L) {
        long t = (a + 50000L) / 100000L;
        snprintf(buf, sizeof buf, "%ld.%ldM", t / 10, t % 10);
    } else {
        long h = (a + 5000000L) / 10000000L;
        snprintf(buf, sizeof buf, "%ld.%02ldB", h / 100, h % 100);
    }
    return buf;
}

static const char *fmt_cop_full(long n) {
    static char buf[16];
    char ib[16];
    long a = n < 0 ? -n : n;
    if (a < 1000000L) {
        fmt_int(ib, sizeof ib, n);
        snprintf(buf, sizeof buf, "%s%s", pf_sign(n), ib);
    } else {
        long h = (a + 5000L) / 10000L;
        snprintf(buf, sizeof buf, "%s%ld.%02ldM", pf_sign(n), h / 100, h % 100);
    }
    return buf;
}

static const char *fmt_cop_short(long n) {
    static char buf[16];
    char ib[16];
    long a = n < 0 ? -n : n;
    if (a < 1000L) {
        fmt_int(ib, sizeof ib, a);
        snprintf(buf, sizeof buf, "%s%s", pf_sign(n), ib);
    } else if (a < 10000L) {
        long k = (a + 50L) / 100L;
        snprintf(buf, sizeof buf, "%s%ld.%ldk", pf_sign(n), k / 10, k % 10);
    } else if (a < 1000000L) {
        long k = (a + 500L) / 1000L;
        if (k >= 1000L) {
            long m = (a + 50000L) / 100000L;
            snprintf(buf, sizeof buf, "%s%ld.%ldM", pf_sign(n), m / 10, m % 10);
        } else {
            snprintf(buf, sizeof buf, "%s%ldk", pf_sign(n), k);
        }
    } else {
        long m = (a + 50000L) / 100000L;
        snprintf(buf, sizeof buf, "%s%ld.%ldM", pf_sign(n), m / 10, m % 10);
    }
    return buf;
}

static const char *fmt_pct_row(int cents) {
    static char buf[12];
    long a = cents < 0 ? -(long)cents : cents;
    snprintf(buf, sizeof buf, "%s%ld.%02ld%%", pf_sign(cents), a / 100, a % 100);
    return buf;
}

static const char *fmt_pct_day(int dp, long dc) {
    static char buf[12];
    long a = dp < 0 ? -(long)dp : dp;
    if (dc == 0) {
        snprintf(buf, sizeof buf, "0.00%%");
    } else {
        snprintf(buf, sizeof buf, "%s%ld.%02ld%%", pf_sign(dc), a / 100, a % 100);
    }
    return buf;
}

static const char *fmt_price(long p) {
    static char buf[16];
    fmt_int(buf, sizeof buf, p);
    return buf;
}

// The firmware measures the real string in px and compares it against a budget
// the row layout computed; it no longer truncates to a fixed character count.
// LVGL's font metrics are not available on the host, so glyph_w() stands in for
// lv_font_get_glyph_width with a table shaped like Styrene 16: wide caps, a
// narrow period. The absolute numbers are an approximation — what the test
// pins down is the *shape* of the behaviour, not the pixel count.
static int glyph_w(char c) {
    if (c == '.') return 4;
    if (c >= '0' && c <= '9') return 9;
    return 10;  // caps and the dash in "GRUPO-SURA" style names
}

static int text_w(const char *s) {
    int w = 0;
    for (const char *p = s; *p; p++) w += glyph_w(*p);
    return w;
}

static const char *fmt_sym(const char *sym, int budget) {
    static char buf[16];
    int n = (int)strlen(sym);
    if (n == 0) { buf[0] = '\0'; return buf; }
    if (text_w(sym) <= budget) { strlcpy(buf, sym, sizeof buf); return buf; }
    // Below three letters the row stops identifying the share, so it is not
    // drawn at all; the caller's guard is what makes room for it.
    if (n <= 3) { buf[0] = '\0'; return buf; }
    for (n = n - 1; ; n--) {
        snprintf(buf, sizeof buf, "%.*s.", n, sym);
        if (n <= 3 || text_w(buf) <= budget) return buf;
    }
}

static const char *fmt_clock(const char *t) {
    static char buf[8];
    if (!t || strlen(t) < 8) { strlcpy(buf, "--:--", sizeof buf); return buf; }
    snprintf(buf, sizeof buf, "%.2s:%.2s", t + 4, t + 6);
    return buf;
}

static const char *fmt_session_date(const char *t) {
    static const char *const months[] = { "ene", "feb", "mar", "abr", "may", "jun",
                                          "jul", "ago", "sep", "oct", "nov", "dic" };
    static char buf[10];
    if (!t || strlen(t) < 8) { strlcpy(buf, "-- ---", sizeof buf); return buf; }
    int mm = (t[0] - '0') * 10 + (t[1] - '0');
    int dd = (t[2] - '0') * 10 + (t[3] - '0');
    if (mm < 1 || mm > 12) { strlcpy(buf, "-- ---", sizeof buf); return buf; }
    snprintf(buf, sizeof buf, "%d %s", dd, months[mm - 1]);
    return buf;
}
// ---- end of copy ----

static int fails = 0;
static void eq(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) == 0) {
        printf("  ok   %-22s %s\n", what, got);
    } else {
        printf("  FAIL %-22s got '%s' want '%s'\n", what, got, want);
        fails++;
    }
}

int main(void) {
    printf("market value (spec §7, no sign, never coloured)\n");
    eq("845,300",  fmt_market_value(845300),      "845,300");
    eq("91.4M",   fmt_market_value(91355200),    "91.4M");
    eq("1.33B",    fmt_market_value(1330000000),   "1.33B");
    eq("999.9M",   fmt_market_value(999900000),    "999.9M");
    eq("999.9M",   fmt_market_value(999949999),    "999.9M");
    eq("1.00B",    fmt_market_value(999950000),    "1.00B");
    eq("0",        fmt_market_value(0),            "0");
    eq("1.0M",     fmt_market_value(1000000),      "1.0M");

    printf("day change / full contribution (sign always on, max 8)\n");
    eq("-523,141",  fmt_cop_full(-523141),   "-523,141");
    eq("0",        fmt_cop_full(0),        "0");
    eq("+812,400", fmt_cop_full(812400),   "+812,400");
    eq("-1.33M",   fmt_cop_full(-1330000), "-1.33M");
    eq("+4,925", fmt_cop_full(4925),   "+4,925");
    eq("-18,368", fmt_cop_full(-18368),  "-18,368");

    printf("short contribution (compact + small board, max 5)\n");
    eq("+840",     fmt_cop_short(840),      "+840");
    eq("+8.4k",    fmt_cop_short(8400),     "+8.4k");
    eq("+4.9k",    fmt_cop_short(4925),   "+4.9k");
    eq("+18k",     fmt_cop_short(18368),    "+18k");
    eq("-18k",    fmt_cop_short(-18368),  "-18k");
    eq("-155k",     fmt_cop_short(-154597),   "-155k");
    eq("-1.5M",    fmt_cop_short(-1545970), "-1.5M");
    eq("+840k",    fmt_cop_short(840000),   "+840k");
    // round-then-unit: 999,600 must not print as "1000k"
    eq("+1.0M",    fmt_cop_short(999600),   "+1.0M");

    printf("row percent (max 7)\n");
    eq("+1.28%",   fmt_pct_row(128),    "+1.28%");
    eq("-1.06%",   fmt_pct_row(-106),   "-1.06%");
    eq("+0.54%",   fmt_pct_row(54),     "+0.54%");
    eq("-10.25%",  fmt_pct_row(-1025),  "-10.25%");

    printf("day percent (sign from dc, never from dp)\n");
    eq("-0.57%",   fmt_pct_day(-57, -523141),   "-0.57%");
    eq("+1.10%",   fmt_pct_day(110, 812400),   "+1.10%");
    eq("0.00%",    fmt_pct_day(0, 0),          "0.00%");
    eq("0.00%",    fmt_pct_day(-3, 0),         "0.00%");
    eq("-0.00%",   fmt_pct_day(0, -523141),     "-0.00%");
    eq("+0.00%",   fmt_pct_day(0, 100),        "+0.00%");
    eq("-10.25%",  fmt_pct_day(-1025, -1545970), "-10.25%");

    printf("price (never abbreviated, no sign)\n");
    eq("24,500",   fmt_price(24500),    "24,500");
    eq("375",      fmt_price(375),      "375");
    eq("122,900",  fmt_price(122900),   "122,900");

    printf("symbol (ASCII dot, never U+2026; gives up below three letters)\n");
    // 11 caps = 110px; every cut is the widest one that still fits the budget.
    eq("fits 110",     fmt_sym("LONGNAMEGRP", 110), "LONGNAMEGRP");
    eq("one over",     fmt_sym("LONGNAMEGRP", 109), "LONGNAMEGR.");
    eq("tight",        fmt_sym("LONGNAMEGRP", 100), "LONGNAMEG.");
    eq("very tight",   fmt_sym("LONGNAMEGRP",  60), "LONGN.");
    eq("three letters",fmt_sym("LONGNAMEGRP",  34), "LON.");
    eq("short fits",   fmt_sym("ALPHA",        60), "ALPHA");
    eq("3 chars cut",  fmt_sym("AXI",          20), "");
    eq("empty in",     fmt_sym("",            100), "");

    printf("timestamp (MMDDhhmm, Colombia)\n");
    eq("10:42",    fmt_clock("09291042"),        "10:42");
    eq("16:00",    fmt_clock("09291600"),        "16:00");
    eq("bad",      fmt_clock("09"),              "--:--");
    eq("29 sep",   fmt_session_date("09291600"), "29 sep");
    eq("9 oct",    fmt_session_date("10091200"), "9 oct");
    eq("28 sep",   fmt_session_date("09281600"), "28 sep");
    eq("bad",      fmt_session_date(""),         "-- ---");

    printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
