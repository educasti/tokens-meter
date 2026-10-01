#include "ui_portfolio.h"
#include "pf_privacy.h"
#include <lvgl.h>
#include <string.h>

#include "hal/board_caps.h"

// Styrene B Regular (SIL OFL 1.1), the same compiled cuts the Claude screens
// use. The seven ASCII cuts cover 0x20-0x7E and nothing else: no U+00B7, no
// U+2026, no tildes and no ñ. Two consequences that shape this file (spec
// §6.2): the clause separator is a comma, and a symbol that does not fit is cut
// to N characters plus an ASCII '.', never an ellipsis. A missing glyph is not
// a hole in this build either — LV_USE_FONT_PLACEHOLDER is on, so it would be
// drawn as a rectangle.
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);

// Portfolio design tokens (design spec §4). Local to this file on purpose: the
// Claude screens keep theme.h. Two cards, no 1 px outline and no chip fill:
// `border` and `element` are gone, the cards are rounded surfaces instead.
// Deliberate deviation from the OpenCode spec: `primary` is only ever words
// starting with "!" and always on the black background, never inside a card —
// on this screen red and green mean "the money went up / down" and nothing
// else, and a number and a notice never share a surface.
#define PF_SURFACE   lv_color_hex(0x1a1a1a)   // the two cards
#define PF_TEXT      lv_color_hex(0xeeeeee)   // market value, symbols, live dot
#define PF_MUTED     lv_color_hex(0x8c8c8c)   // labels, share price, session state
#define PF_PRIMARY   lv_color_hex(0xfab283)   // "!" notices only, on black
#define PF_ERROR     lv_color_hex(0xe06c75)   // negative money
#define PF_SUCCESS   lv_color_hex(0x2ee88a)   // positive money

// Freshness is the firmware's own: the daemon's heartbeat deliberately does not
// replay `pf`, which is what lets "Sin actualizar" ever appear. It only ever
// changes the status text when s == "l", and it never dims anything.
#define PF_FRESH_MS  300000u
#define PF_ROWS      4
// The shortest symbol a row is allowed to show (spec §2.3.1). Its width is
// measured from the row font at layout time, never hardcoded: 54 px at Styrene
// 16. This is the floor the guard compares a row's budget against before it
// starts dropping columns.
#define PF_MIN_SYM   "WWW."

// ---- Text measurement --------------------------------------------------------
// The width LVGL is going to draw, in px. The spec asks for
// lv_text_get_width(), which is the right call: it sums the *real* per-glyph
// advances, so the ±1-2 px that a table of widths leaves out (Styrene rounds
// every glyph on its own, lv_font_fmt_txt.c: `(adv_w + 8) >> 4`) disappear —
// what is measured is what is drawn. In LVGL 9 that function takes a private
// lv_text_attributes_t, so the same sum is done here over the public per-glyph
// API it calls once per letter. Every string on this screen is ASCII by rule,
// so walking bytes is walking characters.
static int32_t text_w(const char *s, const lv_font_t *f) {
    int32_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        w += lv_font_get_glyph_width(f, *p, p[1]);   // p[1] feeds the kerning
    return w;
}

// ---- Layout -----------------------------------------------------------------
// The `LT` table of the design spec §2.3, verbatim: one block per board, all in
// px, `top` measured from the card's top edge. Every `*Ri` key is an inset from
// the card's right edge (CSS `right: Npx`) and never a distance from the left,
// so a card wider or narrower than the one the table was drawn for can never
// push a number off it (§2.2). The card width is always `scr_w - 2 * card_x`,
// which is what lets the 410x502 AMOLED-2.06 fall into `compacto` on the
// breakpoint below and still be laid out from the right.
struct RowFields {
    int16_t sym_x, gap;                 // symbol origin and the minimum air between columns
    int16_t price_ri, pct_ri, contrib_ri;   // insets from the card's right edge; -1 = no column
};

struct PfLayout {
    int16_t scr_w, scr_h;

    int16_t header_x, header_top;
    const lv_font_t *header_font;

    int16_t card_x, card_w, pad;
    int16_t c1_y, c1_h, c1_radius;
    int16_t c2_y, c2_h, c2_radius;

    // card 1
    bool     has_label;                 // the small board has no card-1 label
    int16_t  label_top;  const lv_font_t *label_font;
    int16_t  unit_top, unit_ri;  const lv_font_t *unit_font;   // "COP"
    int16_t  hero_top;  const lv_font_t *hero_font;            // total, or the day's percent
    bool     has_cols;                  // two stacked columns; false = the small board's day line
    int16_t  col_label_top, col_val_top;
    const lv_font_t *col_label_font, *col_val_font;
    int16_t  col1_x, col2_x;            // left edge of each column
    int16_t  day_top, day_dc_x, day_pct_ri;   // the small board's single day line

    // card 2
    int16_t  title_top;  const lv_font_t *title_font;  int16_t count_ri;
    int16_t  rows_top, rows_pitch, rows_gap;
    const lv_font_t *rows_font;         // Styrene 16 on every board, never 12
    RowFields f, f_priv;
    int16_t  norows_top;  const lv_font_t *norows_font;

    // card 1 in an empty state
    int16_t  e_h, e1_top, e2_top, e3_top;
    const lv_font_t *e1_font, *e2_font;
    bool     short_paths;               // the small board shortens the config path

    // status line: on the screen, on black
    int16_t  st_x, st_top, st_dot, st_dot_y, st_text_x;
    const lv_font_t *st_font;

    // private-mode deltas (privacy spec §7)
    int16_t pv_c1_h, pv_c2_y;
};
static PfLayout L = {};

// Three blocks, the same three breakpoints ui.cpp uses. The large one needs
// both dimensions: the AMOLED-2.06 is 502 px tall, so `H >= 460` alone would
// hand it the 480 px layout on a 370 px card, and the numbers defined from the
// left used to run off it.
static void compute_layout(const BoardCaps &c) {
    L.scr_w = c.width;
    L.scr_h = c.height;

    if (c.width >= 460 && c.height >= 460) {
        // ---- grande: 480x480 (AMOLED-2.16, LCD-4, sim) ----
        L.header_x = 20; L.header_top = 36;  L.header_font = &font_styrene_16;
        L.card_x = 20;  L.pad = 20;
        L.c1_y = 72;  L.c1_h = 172; L.c1_radius = 12;      // 72..244
        L.c2_y = 256; L.c2_h = 180; L.c2_radius = 12;      // 256..436
        L.has_label = true;
        L.label_top = 18; L.label_font = &font_styrene_16;
        L.unit_top = 18; L.unit_ri = 20; L.unit_font = &font_styrene_16;
        L.hero_top = 40; L.hero_font = &font_styrene_48;
        L.has_cols = true;
        L.col_label_top = 104; L.col_val_top = 124;
        L.col_label_font = &font_styrene_16; L.col_val_font = &font_styrene_28;
        L.col1_x = 20; L.col2_x = 240;
        L.day_top = 0; L.day_dc_x = 0; L.day_pct_ri = 0;
        L.title_top = 18; L.title_font = &font_styrene_16; L.count_ri = 20;
        L.rows_top = 48; L.rows_pitch = 28; L.rows_gap = 10;
        L.rows_font = &font_styrene_16;
        L.f = { 20, 16, 199, 118,  20 };
        L.f_priv = { 20, 16, 101,  20, -1 };
        L.norows_top = 48; L.norows_font = &font_styrene_16;
        L.e_h = 114; L.e1_top = 20; L.e2_top = 58; L.e3_top = 80;
        L.e1_font = &font_styrene_24; L.e2_font = &font_styrene_14;
        L.short_paths = false;
        L.st_x = 20; L.st_top = 448; L.st_dot = 8; L.st_dot_y = 452; L.st_text_x = 36;
        L.st_font = &font_styrene_16;
        L.pv_c1_h = 109; L.pv_c2_y = 193;
    } else if (c.height >= 300) {
        // ---- compacto: 368x448 (AMOLED-1.8) and 410x502 (AMOLED-2.06) ----
        L.header_x = 20; L.header_top = 32;  L.header_font = &font_styrene_14;
        L.card_x = 20;  L.pad = 16;
        L.c1_y = 64;  L.c1_h = 150; L.c1_radius = 12;      // 64..214
        L.c2_y = 224; L.c2_h = 166; L.c2_radius = 12;      // 224..390
        L.has_label = true;
        L.label_top = 14; L.label_font = &font_styrene_14;
        L.unit_top = 14; L.unit_ri = 16; L.unit_font = &font_styrene_14;
        L.hero_top = 32; L.hero_font = &font_styrene_48;
        L.has_cols = true;
        L.col_label_top = 94; L.col_val_top = 111;
        L.col_label_font = &font_styrene_14; L.col_val_font = &font_styrene_24;
        L.col1_x = 16; L.col2_x = 164;
        L.day_top = 0; L.day_dc_x = 0; L.day_pct_ri = 0;
        L.title_top = 14; L.title_font = &font_styrene_14; L.count_ri = 16;
        L.rows_top = 40; L.rows_pitch = 28; L.rows_gap = 8;
        L.rows_font = &font_styrene_16;
        // No contribution: it does not fit next to the price on 328 px, and the
        // price is the column the user checks against the broker (§2.1).
        L.f = { 16, 12, 106, 16, -1 };
        L.f_priv = { 16, 12, 106, 16, -1 };
        L.norows_top = 40; L.norows_font = &font_styrene_14;
        L.e_h = 94; L.e1_top = 16; L.e2_top = 46; L.e3_top = 64;
        L.e1_font = &font_styrene_20; L.e2_font = &font_styrene_12;
        L.short_paths = false;
        L.st_x = 20; L.st_top = 404; L.st_dot = 8; L.st_dot_y = 407; L.st_text_x = 34;
        L.st_font = &font_styrene_14;
        L.pv_c1_h = 98; L.pv_c2_y = 172;
    } else {
        // ---- chico: 240x240 (compact breakpoint, currently unused) ----
        L.header_x = 8; L.header_top = 10;  L.header_font = &font_styrene_12;
        L.card_x = 8;  L.pad = 8;
        L.c1_y = 30;  L.c1_h = 63;  L.c1_radius = 8;        // 30..93
        L.c2_y = 99;  L.c2_h = 109; L.c2_radius = 8;        // 99..208
        L.has_label = false;           // no room for it: the number is the label
        L.label_top = 0; L.label_font = &font_styrene_12;
        L.unit_top = 19; L.unit_ri = 8; L.unit_font = &font_styrene_12;
        L.hero_top = 6; L.hero_font = &font_styrene_28;
        L.has_cols = false;            // the day change is one 16 px line
        L.col_label_top = 0; L.col_val_top = 0;
        L.col_label_font = &font_styrene_12; L.col_val_font = &font_styrene_16;
        L.col1_x = 0; L.col2_x = 0;
        L.day_top = 40; L.day_dc_x = 8; L.day_pct_ri = 8;
        L.title_top = 6; L.title_font = &font_styrene_12; L.count_ri = 8;
        L.rows_top = 24; L.rows_pitch = 19; L.rows_gap = 4;
        L.rows_font = &font_styrene_16;
        // Symbol and percent only. The price would need 71 px that are not here
        // (§2.1), and a column that comes and goes with the day's prices is not
        // a column.
        L.f = { 8, 10, -1, 8, -1 };
        L.f_priv = { 8, 10, -1, 8, -1 };
        L.norows_top = 24; L.norows_font = &font_styrene_12;
        L.e_h = 64; L.e1_top = 6; L.e2_top = 28; L.e3_top = 44;
        L.e1_font = &font_styrene_16; L.e2_font = &font_styrene_12;
        L.short_paths = true;
        L.st_x = 8; L.st_top = 214; L.st_dot = 6; L.st_dot_y = 218; L.st_text_x = 20;
        L.st_font = &font_styrene_12;
        L.pv_c1_h = 42; L.pv_c2_y = 78;
    }

    // Cards span the screen minus both margins, so a board on any breakpoint
    // with a panel narrower than the one the table was drawn for keeps every
    // inset inside it.
    L.card_w = L.scr_w - 2 * L.card_x;
}

// ---- Widgets ----------------------------------------------------------------
// Object budget: the shared LVGL pool is 96 KB on every board, and the four
// pre-existing screens already hold ~48 KB of it — the C6 boards have no PSRAM
// to grow it into. So the widget set here is kept as small as the spec allows:
// the market value and the private hero are the same label, the two cards are
// dimmed directly instead of through an extra full-screen group, the 1 px
// divider is gone (the gap between the groups is the air), and the row cells are
// the only per-row objects. A column only gets labels on the boards whose table
// has it, so the small board never pays for a price it will not draw.
static lv_obj_t *root;          // screen container
static lv_obj_t *header_lbl;
static lv_obj_t *card1, *card2;
static lv_obj_t *lbl_label, *lbl_unit, *lbl_hero;
static lv_obj_t *lbl_col1, *lbl_col2, *lbl_dc, *lbl_dc_pct;
static lv_obj_t *lbl_e[3];
static lv_obj_t *lbl_title, *lbl_count, *lbl_norows;
static lv_obj_t *row_sym[PF_ROWS], *row_price[PF_ROWS];
static lv_obj_t *row_pct[PF_ROWS], *row_contrib[PF_ROWS];
static lv_obj_t *status_dot, *status_lbl;

// ---- State ------------------------------------------------------------------
static PfData   cur;
static bool     have_data = false;   // a valid payload arrived since boot
static uint32_t data_ms = 0;
static bool     ble_on = false;
static bool     dimmed = false;
static int      mode_applied = -1;    // -1 unknown / 0 normal / 1 private
static int16_t  card1_h = -1;         // card 1 height currently pushed to LVGL
static bool     sq_shown = false;     // the fixed "En vivo" dot
static int16_t   min_sym_w = 0;       // W("WWW.") in the row font
// Both start out "unset" (black is never a status colour, -1 is no x) so the
// first draw always pushes them; after that only real changes reach LVGL.
static lv_color_t status_color;
static int16_t   status_x = -1;
static char      status_text[48] = "";

// One row's text, measured. Static so the two layout passes below can share it
// without a 250-byte frame in a redraw that already runs on the LVGL task's
// stack. Nothing in here is state that has to survive a redraw.
struct RowGeom {
    char    sym[PF_SYM_MAX + 2];
    char    price[16];
    char    pct[12];
    char    contrib[16];
    int16_t w_price, w_pct, w_contrib;
    int16_t r_contrib, r_pct, r_price;  // right edges inside the card
    int16_t top;
};
static RowGeom rowg[PF_ROWS];

static void redraw(void);

// ---- Formatting (portfolio spec §7) ---------------------------------------
//
// Comma for thousands, dot for decimals — the broker's own format, so the
// screen matches it digit by digit. '+' / ASCII '-' / nothing for zero. The
// rounding always happens *before* the unit is chosen, so 999,600 can never
// print as "1000k". No float, no U+2212, no accounting parentheses.

static const char *pf_sign(long n) {
    return n > 0 ? "+" : n < 0 ? "-" : "";
}

// Absolute value with a comma every three digits (the sign is the caller's).
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
        // 66260 -> "66,260", 122960 -> "122,960", 375 -> "375".
        if (i < n - 1 && ((i + 1) % 3) == 0 && o + 1 < len) buf[o++] = ',';
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
}

// Market value: a level, so never a sign and never a colour.
//   "845,300" · "132.9M" · "1.33B"
static const char *fmt_market_value(long mv) {
    static char buf[16];
    long a = mv < 0 ? -mv : mv;
    if (a < 1000000L) {
        fmt_int(buf, sizeof buf, mv);
    } else if (a < 999950000L) {
        long t = (a + 50000L) / 100000L;          // tenths of a million
        snprintf(buf, sizeof buf, "%ld.%ldM", t / 10, t % 10);
    } else {
        long h = (a + 5000000L) / 10000000L;       // hundredths of a billion
        snprintf(buf, sizeof buf, "%ld.%02ldB", h / 100, h % 100);
    }
    return buf;
}

// COP with sign: "< 1M full with commas, else X.XXM" — the day change and the
// row contribution.   "-34,880" · "+812,400" · "-1.33M"
static const char *fmt_cop_full(long n) {
    static char buf[16];
    char ib[16];
    long a = n < 0 ? -n : n;
    if (a < 1000000L) {
        fmt_int(ib, sizeof ib, n);
        snprintf(buf, sizeof buf, "%s%s", pf_sign(n), ib);
    } else {
        long h = (a + 5000L) / 10000L;            // hundredths of a million
        snprintf(buf, sizeof buf, "%s%ld.%02ldM", pf_sign(n), h / 100, h % 100);
    }
    return buf;
}

// Row percent: sign + 2 decimals + "%" (max 7).   "+1.28%" · "-10.25%"
static const char *fmt_pct_row(int cents) {
    static char buf[12];
    long a = cents < 0 ? -(long)cents : cents;
    snprintf(buf, sizeof buf, "%s%ld.%02ld%%", pf_sign(cents), a / 100, a % 100);
    return buf;
}

// Day percent: the sign comes from `dc`, never from `dp`, so a day whose
// percent rounds to zero still shows the true sign ("-0.00%").
//   "-0.03%" · "+1.10%" · "0.00%" when dc == 0
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

// Per-share price: full, never abbreviated — the price is compared against the
// broker and a "k" would erase exactly the tick that changed. It is also the
// one peso figure that stays in private mode: it is a public quote, identical
// for anybody holding one share or a hundred thousand (privacy spec §7.1).
static const char *fmt_price(long p) {
    static char buf[16];
    fmt_int(buf, sizeof buf, p);
    return buf;
}

// Symbol without its .CL suffix, cut to the first N characters that fit in the
// row's own budget plus a '.' (spec §6.2.3). The cut is by measured width, not
// by a character count, because Styrene is proportional: 'I' and 'W' are not the
// same size. The dot is the Spanish abbreviation mark, it exists in every cut,
// and it is what keeps "PFGRUPOAR." from reading as a ticker that exists. It is
// never an ellipsis: these fonts have no U+2026 and a missing glyph would be
// drawn as a rectangle. Three letters is the floor — below that the row stops
// identifying the share — so a name that does not fit is not drawn at all, and
// the caller's guard (§2.3.1) is what makes room for it.
static const char *fmt_sym(const char *sym, int16_t budget, char *out, size_t len) {
    int n = (int)strlen(sym);
    if (n == 0) { out[0] = '\0'; return out; }
    if (text_w(sym, L.rows_font) <= budget) { strlcpy(out, sym, len); return out; }
    // It does not fit, and a name cut below three letters would stop identifying
    // the share, so it is not drawn at all: the caller's guard is what makes
    // room for it, by dropping a column.
    if (n <= 3) { out[0] = '\0'; return out; }
    for (n = n - 1; ; n--) {
        snprintf(out, len, "%.*s.", n, sym);
        if (n <= 3 || text_w(out, L.rows_font) <= budget) return out;
    }
}

// t = "MMDDhhmm", Colombia time of the last price: "10:42"
static const char *fmt_clock(const char *t) {
    static char buf[8];
    if (!t || strlen(t) < 8) { strlcpy(buf, "--:--", sizeof buf); return buf; }
    snprintf(buf, sizeof buf, "%.2s:%.2s", t + 4, t + 6);
    return buf;
}

// Same timestamp as a session close: "29 sep" (day without a leading zero)
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

// Green above, red below, plain white at zero — and it only ever touches a
// signed number.
static lv_color_t money_color(long n) {
    return n > 0 ? PF_SUCCESS : n < 0 ? PF_ERROR : PF_TEXT;
}

// ---- The row: right-anchored, measured, surplus to the symbol (spec §2.3.1) ---
//
// Composed from the right edge inwards, once per row, with the widths of the
// strings that are actually going to be drawn:
//
//   cR  = cardW - contribRi            (always against the padding: it is the
//   cL  = cR - W(contrib)              rightmost column, nothing can push it)
//   pR  = min(cardW - pctRi,  cL - gap)
//   pL  = pR - W(pct)
//   prR = min(cardW - priceRi, pL - gap)
//   prL = prR - W(price)
//   symBudget = prL - gap - symX       (whatever is left belongs to the symbol)
//
// Two properties fall out of the `min` and are the whole point of the change:
// the default insets keep the columns aligned from row to row, and a value
// wider than expected only ever pushes the columns to its left, never over
// them. The leftover goes to the symbol, which is cut to fit. The margin of a
// row is therefore "how many px the numbers can still grow", not "how much was
// left over of a constant".
static int16_t row_layout(const RowGeom *g, const RowFields *f, bool with_c, bool with_pr,
                          int16_t *r_c, int16_t *r_p, int16_t *r_pr) {
    int32_t left = INT32_MAX;                       // left edge of the column to the right
    if (with_c) {
        int32_t r = (int32_t)L.card_w - f->contrib_ri;
        *r_c = (int16_t)r;
        left = r - g->w_contrib;
    }
    int32_t p = (int32_t)L.card_w - f->pct_ri;
    if (p > left - f->gap) p = left - f->gap;       // INT32_MAX leaves the default alone
    *r_p = (int16_t)p;
    int32_t p_left = p - g->w_pct;
    left = p_left;
    if (with_pr) {
        int32_t r = (int32_t)L.card_w - f->price_ri;
        if (r > p_left - f->gap) r = p_left - f->gap;
        *r_pr = (int16_t)r;
        left = r - g->w_price;
    }
    return (int16_t)(left - f->gap - f->sym_x);
}

// The tightest symbol budget across the rows on screen, which is what the guard
// compares against. Pass 1 has already formatted and measured every row, so this
// is the same arithmetic the placement below does, without touching it.
static int16_t tightest_budget(const RowFields *f, int n, bool with_c, bool with_pr) {
    int16_t tightest = INT16_MAX;
    for (int i = 0; i < n; i++) {
        int16_t r_c, r_p, r_pr;
        int16_t b = row_layout(&rowg[i], f, with_c, with_pr, &r_c, &r_p, &r_pr);
        if (b < tightest) tightest = b;
    }
    return tightest;
}

// ---- Builders ---------------------------------------------------------------

// Transparent, borderless, click-through group. EVENT_BUBBLE everywhere so a
// tap anywhere reaches the screen root, where ui.cpp listens.
static lv_obj_t *make_group(lv_obj_t *parent) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_size(g, L.scr_w, L.scr_h);
    lv_obj_set_pos(g, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_EVENT_BUBBLE);
    return g;
}

// A card: a rounded dark surface with no outline. The grouping is the fill, not
// a line, so radius 12 / 12 / 8 and border_width 0 (spec §4). Children are
// positioned by hand.
static lv_obj_t *make_card(lv_obj_t *parent, int16_t y, int16_t h, int16_t radius) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, L.card_x, y);
    lv_obj_set_size(p, L.card_w, h);
    lv_obj_set_style_bg_color(p, PF_SURFACE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
    return p;
}

static lv_obj_t *make_label(lv_obj_t *parent, int16_t x, int16_t y,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(l, 0, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);   // one line, like nowrap
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

// Place a label with its *right edge* `inset` px in from the card's right edge
// (CSS `right: Npx`). lv_obj_align(TOP_RIGHT, x_ofs) moves the object outward
// by x_ofs, so the inset has to be negated.
static void place_right(lv_obj_t *l, int16_t inset, int16_t top) {
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -inset, top);
}

// Same, from a right edge the row algorithm computed: `r` is measured from the
// card's left edge, so the inset is whatever is left between it and the card's
// right edge. This is the only way a column gets placed — there is no
// left-to-right composition and no distance-from-the-left anywhere (spec §2.2).
static void place_at(lv_obj_t *l, int16_t r, int16_t top) {
    place_right(l, L.card_w - r, top);
}

// Null-safe: a board that has no price column never gets an object for it.
static void set_vis(lv_obj_t *l, bool on) {
    if (!l) return;
    if (on) lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
}

void pf_usage_init(lv_obj_t *parent) {
    compute_layout(board_caps());
    min_sym_w = (int16_t)text_w(PF_MIN_SYM, L.rows_font);

    root = make_group(parent);
    header_lbl = make_label(root, L.header_x, L.header_top, L.header_font, PF_MUTED);

    // The battery belongs to ui.cpp and stays on top of this screen, like on
    // the two usage screens.
    card1 = make_card(root, L.c1_y, L.c1_h, L.c1_radius);
    card2 = make_card(root, L.c2_y, L.c2_h, L.c2_radius);

    // Card 1. "Valor total" (or "Cambio hoy" in private) and "COP" frame the
    // hero from above; the two columns sit under it, labelled or not depending
    // on the board. The small board has no label and shows the day change as a
    // single 16 px line instead of two columns.
    lbl_label = make_label(card1, L.pad, L.label_top, L.label_font, PF_MUTED);
    lbl_unit  = make_label(card1, 0, L.unit_top, L.unit_font, PF_MUTED);
    place_right(lbl_unit, L.unit_ri, L.unit_top);
    lbl_hero  = make_label(card1, L.pad, L.hero_top, L.hero_font, PF_TEXT);
    if (L.has_cols) {
        lbl_col1 = make_label(card1, L.col1_x, L.col_label_top, L.col_label_font, PF_MUTED);
        lbl_col2 = make_label(card1, L.col2_x, L.col_label_top, L.col_label_font, PF_MUTED);
        lv_label_set_text(lbl_col1, "Cambio hoy");
        lv_label_set_text(lbl_col2, "Cambio %");
        lbl_dc     = make_label(card1, L.col1_x, L.col_val_top, L.col_val_font, PF_TEXT);
        lbl_dc_pct = make_label(card1, L.col2_x, L.col_val_top, L.col_val_font, PF_TEXT);
    } else {
        lbl_dc     = make_label(card1, L.day_dc_x, L.day_top, L.col_val_font, PF_TEXT);
        lbl_dc_pct = make_label(card1, 0, L.day_top, L.col_val_font, PF_TEXT);
        place_right(lbl_dc_pct, L.day_pct_ri, L.day_top);
    }
    for (int i = 0; i < 3; i++)
        lbl_e[i] = make_label(card1, L.pad, L.e1_top,
                              i == 0 ? L.e1_font : L.e2_font,
                              i == 0 ? PF_TEXT : PF_MUTED);

    // Card 2: the movers. The title and the share count share a line, with the
    // count against the right padding.
    lbl_title  = make_label(card2, L.pad, L.title_top, L.title_font, PF_MUTED);
    lbl_count  = make_label(card2, 0, L.title_top, L.title_font, PF_MUTED);
    place_right(lbl_count, L.count_ri, L.title_top);
    lbl_norows = make_label(card2, L.pad, L.norows_top, L.norows_font, PF_MUTED);

    for (int i = 0; i < PF_ROWS; i++) {
        row_sym[i] = make_label(card2, L.f.sym_x, 0, L.rows_font, PF_TEXT);
        if (L.f.price_ri >= 0)
            row_price[i] = make_label(card2, 0, 0, L.rows_font, PF_MUTED);
        row_pct[i] = make_label(card2, 0, 0, L.rows_font, PF_TEXT);
        if (L.f.contrib_ri >= 0)
            row_contrib[i] = make_label(card2, 0, 0, L.rows_font, PF_TEXT);
    }

    // Status line, on root rather than inside a card: it is how the user learns
    // that the numbers above went stale or the link dropped. The dot is the
    // fixed white "En vivo" mark — a drawn circle, not a glyph, and never
    // blinking (a blink is a redraw on the one screen built not to redraw).
    status_dot = lv_obj_create(root);
    lv_obj_set_pos(status_dot, L.st_x, L.st_dot_y);
    lv_obj_set_size(status_dot, L.st_dot, L.st_dot);
    lv_obj_set_style_bg_color(status_dot, PF_TEXT, 0);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_dot, 0, 0);
    lv_obj_set_style_radius(status_dot, L.st_dot / 2, 0);
    lv_obj_set_style_pad_all(status_dot, 0, 0);
    lv_obj_clear_flag(status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(status_dot, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(status_dot, LV_OBJ_FLAG_HIDDEN);

    status_lbl = make_label(root, L.st_x, L.st_top, L.st_font, PF_MUTED);
    lv_label_set_text(status_lbl, "");

    // Neutral state, so the screen is never blank if it is shown before the
    // first payload (ui.cpp keeps it out of the cycle until then).
    memset(&cur, 0, sizeof(cur));
    cur.ok = true;
    redraw();

    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

// ---- Drawing ----------------------------------------------------------------

// Which mode the screen has to be drawn in right now. Private mode only applies
// to a payload with data: the empty states have nothing private to hide, so they
// keep the normal card box and only the text that reveals `n` changes.
static bool want_private(void) {
    return pf_privacy_get() && cur.ok;
}

// Card 2's position for the current mode. Runs only when the mode actually
// changes, so a redraw with the mode unchanged never touches a position. Card
// 1's height belongs to draw_panel1(), which also has to know about the empty
// states.
static void apply_mode(bool priv) {
    int m = priv ? 1 : 0;
    if (mode_applied == m) return;
    mode_applied = m;
    // Private mode drops the two columns and the unit from card 1, so that
    // card gets shorter and card 2 moves up as a block, keeping its own height
    // (privacy spec §4). The gap left below is the visible sign of the mode and
    // is never filled.
    lv_obj_set_pos(card2, L.card_x, priv ? L.pv_c2_y : L.c2_y);
}

// Two warning zones in the header: "e" outranks "u", and "u" outranks "w"
// (portfolio spec §4). Primary, always prefixed with "!", and always on the
// black background above the first card.
static void draw_header(void) {
    const char *text = "Portfolio";
    lv_color_t color = PF_MUTED;
    if (cur.ok && cur.u >= 1) {
        if (cur.ux[0]) {
            static char buf[40];
            if (cur.u > 1) snprintf(buf, sizeof buf, "! %s +%d sin precio", cur.ux, cur.u - 1);
            else            snprintf(buf, sizeof buf, "! %s sin precio", cur.ux);
            text = buf;
        } else {
            static char buf[24];
            snprintf(buf, sizeof buf, "! %d sin precio", cur.u);
            text = buf;
        }
        color = PF_PRIMARY;
    } else if (cur.ok && cur.w >= 1) {
        static char buf[32];
        snprintf(buf, sizeof buf, "! %d aviso, ver log", cur.w);
        text = buf;
        color = PF_PRIMARY;
    }
    lv_label_set_text(header_lbl, text);
    lv_obj_set_style_text_color(header_lbl, color, 0);
}

// Card 1 in its three forms: an empty state, the private hero (the day's
// percent, same size as the market value) or the market value with the day
// change below it in pesos and in percent.
static void draw_panel1(bool priv) {
    int16_t h1 = !cur.ok ? L.e_h : (priv ? L.pv_c1_h : L.c1_h);
    if (h1 != card1_h) {
        card1_h = h1;
        lv_obj_set_size(card1, L.card_w, h1);
    }

    if (!cur.ok) {
        // Line 1 says what happened, lines 2 and 3 how to fix it.
        const char *l1, *l2, *l3;
        if (strcmp(cur.e, "nopos") == 0) {
            l1 = "Sin posiciones";
            // The full path does not fit the 240 card: "config: portfolio" there.
            l2 = L.short_paths ? "config: portfolio"
                               : "~/.config/claude-usage-monitor/portfolio";
            l3 = "Formato: TICKER.CL cantidad";
        } else if (strcmp(cur.e, "nonet") == 0) {
            l1 = "Sin precios"; l2 = "Yahoo no responde"; l3 = "Reintenta cada 60 s";
        } else {   // nores
            l1 = "Sin precios"; l3 = "Revisa el sufijo .CL"; l2 = "";
            // This is the one empty state that reacts to private mode, and it
            // does so even though the payload is ok:false — "0 de 12 con
            // precio" gives away n, which is private (privacy spec §3.2). The
            // card itself stays the normal one: there is nothing else to
            // hide here.
            if (pf_privacy_get()) {
                l2 = "Ninguno con precio";
            } else {
                static char buf[24];
                snprintf(buf, sizeof buf, "0 de %d con precio", cur.n);
                l2 = buf;
            }
        }
        const char *lines[3] = { l1, l2, l3 };
        const int16_t tops[3] = { L.e1_top, L.e2_top, L.e3_top };
        set_vis(lbl_label, false);
        set_vis(lbl_unit, false);
        set_vis(lbl_hero, false);
        set_vis(lbl_dc, false);
        set_vis(lbl_dc_pct, false);
        set_vis(lbl_col1, false);
        set_vis(lbl_col2, false);
        for (int i = 0; i < 3; i++) {
            if (!lines[i] || !lines[i][0]) { set_vis(lbl_e[i], false); continue; }
            lv_label_set_text(lbl_e[i], lines[i]);
            lv_obj_set_pos(lbl_e[i], L.pad, tops[i]);
            lv_obj_set_style_text_color(lbl_e[i], i == 0 ? PF_TEXT : PF_MUTED, 0);
            set_vis(lbl_e[i], true);
        }
        return;
    }

    for (int i = 0; i < 3; i++) set_vis(lbl_e[i], false);

    // "Valor total" names the hero; in private mode there is no total, so the
    // label says what is left, the day's percent. The unit goes away with it:
    // "COP" would describe a number that is no longer on screen.
    lv_label_set_text(lbl_label, priv ? "Cambio hoy" : "Valor total");
    set_vis(lbl_label, L.has_label);
    lv_label_set_text(lbl_unit, "COP");
    set_vis(lbl_unit, !priv);

    if (priv) {
        // The hero is the day's percent: a ratio, so it carries no scale. Its
        // sign comes from dc, which is the same information the colour gives.
        lv_label_set_text(lbl_hero, fmt_pct_day(cur.dp, cur.dc));
        lv_obj_set_style_text_color(lbl_hero, money_color(cur.dc), 0);
    } else {
        // The market value is a level: no colour, no sign.
        lv_label_set_text(lbl_hero, fmt_market_value(cur.mv));
        lv_obj_set_style_text_color(lbl_hero, PF_TEXT, 0);

        // Day change, in pesos and in percent, same colour and same sign. In
        // pesos it is the number the contributions of the list add up to, so
        // the two are directly comparable digit by digit.
        lv_color_t c = money_color(cur.dc);
        lv_label_set_text(lbl_dc, fmt_cop_full(cur.dc));
        lv_obj_set_style_text_color(lbl_dc, c, 0);
        lv_label_set_text(lbl_dc_pct, fmt_pct_day(cur.dp, cur.dc));
        lv_obj_set_style_text_color(lbl_dc_pct, c, 0);
        set_vis(lbl_dc, true);
        set_vis(lbl_dc_pct, true);
    }
    set_vis(lbl_hero, true);
    set_vis(lbl_col1, !priv && L.has_cols);
    set_vis(lbl_col2, !priv && L.has_cols);
    if (priv) { set_vis(lbl_dc, false); set_vis(lbl_dc_pct, false); }
}

// Card 2: the biggest movers. The daemon already sends at most two gainers
// then two losers; the firmware drops any exact 0% (a 0% row says nothing and
// is not a variation) and re-groups by sign so the air between the two groups
// can never land in the middle of one. Never a filler row.
static void draw_panel2(bool priv) {
    set_vis(card2, cur.ok);
    if (!cur.ok) return;

    lv_label_set_text(lbl_title, "Mayores variaciones");
    char cbuf[16];
    snprintf(cbuf, sizeof cbuf, "de %d", cur.n);
    lv_label_set_text(lbl_count, cbuf);
    // "de 12" says how many shares are held: private, so it goes away.
    set_vis(lbl_count, !priv);

    const PfRow *rows[PF_ROWS];
    int n = 0;
    for (int i = 0; i < cur.nr && n < PF_ROWS; i++) {
        if (cur.r[i].pct == 0) continue;
        rows[n++] = &cur.r[i];
    }
    // Stable partition: gainers first, then losers, each keeping the order the
    // daemon sent (best % first, most negative first).
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && rows[j]->pct > 0 && rows[j - 1]->pct < 0; j--) {
            const PfRow *tmp = rows[j]; rows[j] = rows[j - 1]; rows[j - 1] = tmp;
        }
    }
    int g = 0;
    while (g < n && rows[g]->pct > 0) g++;

    const RowFields *f = priv ? &L.f_priv : &L.f;

    if (n == 0) {
        lv_label_set_text(lbl_norows, "Sin cambios por ahora");
        set_vis(lbl_norows, true);
        for (int i = 0; i < PF_ROWS; i++) {
            set_vis(row_sym[i], false); set_vis(row_price[i], false);
            set_vis(row_pct[i], false); set_vis(row_contrib[i], false);
        }
        return;
    }
    set_vis(lbl_norows, false);

    // Pass 1: format every row and measure it with all of its columns on, to
    // find the tightest symbol budget on the screen.
    for (int i = 0; i < n; i++) {
        RowGeom *rg = &rowg[i];
        rg->w_price = 0; rg->w_contrib = 0;
        strlcpy(rg->sym, rows[i]->sym, sizeof rg->sym);
        strlcpy(rg->pct, fmt_pct_row(rows[i]->pct), sizeof rg->pct);
        rg->w_pct = (int16_t)text_w(rg->pct, L.rows_font);
        if (row_price[i]) {
            strlcpy(rg->price, fmt_price(rows[i]->price), sizeof rg->price);
            rg->w_price = (int16_t)text_w(rg->price, L.rows_font);
        }
        // The contribution depends on the share count, so in private mode it is
        // not formatted and not drawn in any of its forms.
        if (row_contrib[i] && f->contrib_ri >= 0) {
            strlcpy(rg->contrib, fmt_cop_full(rows[i]->contrib), sizeof rg->contrib);
            rg->w_contrib = (int16_t)text_w(rg->contrib, L.rows_font);
        }
    }

    // The guard (spec §2.3.1), decided once for the whole screen so the columns
    // stay in the same place from row to row: if what is left cannot hold the
    // shortest symbol worth showing, the contribution goes, then the price. No
    // board can end up with a number drawn on top of another, or with a symbol
    // of fewer than three letters.
    bool show_c = f->contrib_ri >= 0;
    bool show_pr = f->price_ri >= 0;
    if (tightest_budget(f, n, show_c, show_pr) < min_sym_w) {
        show_c = false;
        if (tightest_budget(f, n, show_c, show_pr) < min_sym_w) show_pr = false;
    }

    // Pass 2: place the surviving columns and cut each symbol to its own budget.
    for (int i = 0; i < n; i++) {
        RowGeom *rg = &rowg[i];
        int16_t budget = row_layout(rg, f, show_c, show_pr,
                                    &rg->r_contrib, &rg->r_pct, &rg->r_price);
        rg->top = L.rows_top + i * L.rows_pitch
                + ((g > 0 && i >= g) ? L.rows_gap : 0);
        lv_color_t c = money_color(rows[i]->pct);

        lv_obj_set_pos(row_sym[i], f->sym_x, rg->top);
        set_vis(row_sym[i], budget >= min_sym_w);
        if (budget >= min_sym_w) {
            char cut[PF_SYM_MAX + 2];
            lv_label_set_text(row_sym[i], fmt_sym(rg->sym, budget, cut, sizeof cut));
        }

        // The price is the reference figure, so it is the one column in muted
        // and the only one without a sign: "grey number without a sign = price"
        // is what tells it apart from the movement beside it.
        set_vis(row_price[i], show_pr);
        if (show_pr) {
            lv_label_set_text(row_price[i], rg->price);
            lv_obj_set_style_text_color(row_price[i], PF_MUTED, 0);
            place_at(row_price[i], rg->r_price, rg->top);
        }

        lv_label_set_text(row_pct[i], rg->pct);
        lv_obj_set_style_text_color(row_pct[i], c, 0);
        place_at(row_pct[i], rg->r_pct, rg->top);
        set_vis(row_pct[i], true);

        set_vis(row_contrib[i], show_c);
        if (show_c) {
            lv_label_set_text(row_contrib[i], rg->contrib);
            lv_obj_set_style_text_color(row_contrib[i], c, 0);
            place_at(row_contrib[i], rg->r_contrib, rg->top);
        }
    }
    for (int i = n; i < PF_ROWS; i++) {
        set_vis(row_sym[i], false); set_vis(row_price[i], false);
        set_vis(row_pct[i], false); set_vis(row_contrib[i], false);
    }
}

// Session state and link, on the status line (portfolio spec §4). The transient
// "Modo privado" / "Modo normal" notice has priority: it is what tells a user
// with the "PWR = brightness" habit why the screen just changed.
static void draw_status(uint32_t now) {
    char buf[48];
    const char *text;
    lv_color_t color;
    bool dot = false;

    if (pf_privacy_notice()) {
        text = pf_privacy_get() ? "Modo privado" : "Modo normal";
        color = PF_MUTED;
    } else if (!ble_on) {
        text = "! Bluetooth desconectado";
        color = PF_PRIMARY;
    } else if (!cur.ok) {
        text = "";                    // the empty states have nothing to report
        color = PF_MUTED;
    } else if (cur.live) {
        bool stale = have_data && (now - data_ms) > PF_FRESH_MS;
        if (stale) {
            // Stops saying "En vivo", which was the only false thing on screen.
            // Nothing is dimmed: in the stock market, "old" is the normal state.
            snprintf(buf, sizeof buf, "Sin actualizar desde %s", fmt_clock(cur.t));
            text = buf;
            color = PF_MUTED;
        } else {
            snprintf(buf, sizeof buf, "En vivo, %s", fmt_clock(cur.t));
            text = buf;
            color = PF_TEXT;
            dot = true;
        }
    } else {
        snprintf(buf, sizeof buf, "Cierre %s", fmt_session_date(cur.t));
        text = buf;
        color = PF_MUTED;
    }

    if (strcmp(text, status_text) != 0) {
        lv_label_set_text(status_lbl, text);
        strlcpy(status_text, text, sizeof(status_text));
    }
    if (!lv_color_eq(color, status_color)) {
        status_color = color;
        lv_obj_set_style_text_color(status_lbl, color, 0);
    }
    // The active state indents the text to clear the dot; every other state
    // starts on the screen margin.
    int16_t x = dot ? L.st_text_x : L.st_x;
    if (x != status_x) {
        status_x = x;
        lv_obj_set_pos(status_lbl, x, L.st_top);
    }
    if (dot != sq_shown) {
        sq_shown = dot;
        if (dot) lv_obj_clear_flag(status_dot, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(status_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

// A dropped link is the one case that dims: there the data's own freshness is
// unknown. Everything else on this screen stays at full brightness. At 40% the
// #1a1a1a card lands on about #0a0a0a over black, and the price dims with it
// on purpose: it is the figure that goes stale fastest.
static void apply_dim(void) {
    bool dim = !ble_on;
    if (dim == dimmed) return;
    dimmed = dim;
    // opa is inherited, so the two cards fade as a block while the header and
    // the status line keep full opacity.
    lv_obj_set_style_opa(card1, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
    lv_obj_set_style_opa(card2, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
}

static void redraw(void) {
    if (!root) return;
    bool priv = want_private();
    apply_mode(priv);
    draw_header();
    draw_panel1(priv);
    draw_panel2(priv);
    draw_status(millis());
    apply_dim();
}

// ======== Public API =========================================================

void pf_usage_show(void) {
    if (!root) return;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    redraw();
}

void pf_usage_hide(void) {
    if (!root) return;
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *pf_usage_get_root(void) {
    return root;
}

void pf_usage_update(const PfData *d) {
    if (!d) return;
    cur = *d;
    data_ms = millis();
    if (d->valid) have_data = true;
    redraw();
}

void pf_usage_set_ble(bool connected) {
    ble_on = connected;
    redraw();
}

// Runs from ui_tick_anim() while this screen is visible. Two things are
// time-based — the transient private-mode notice and the "Sin actualizar"
// switch — plus the PWR toggle, which has to redraw from the mode flag. When
// the market is closed and nothing is touched, this pushes nothing to LVGL.
void pf_usage_tick(void) {
    if (!root) return;
    if (want_private() != (mode_applied == 1)) {
        redraw();
        return;
    }
    draw_status(millis());
    apply_dim();
}

bool pf_has_data(void) {
    return have_data;
}
