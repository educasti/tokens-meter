#include "oc_splash.h"
#include "splash.h"
#include "splash_geometry.h"
#include <Arduino.h>
#include <string.h>

// OpenCode splash, ported from the approved prototype
// design/opencode-screen/oc-splash.js: same scenes, same millisecond timings,
// same colours, same scanner frame table. The prototype draws into a canvas
// with alpha; here every drawn colour is the prototype's rgba blend over the
// black stage baked into an RGB565 palette entry once, at compose time, so the
// scene module stays as cheap as the Clawd animations (a 60x60 cell buffer plus
// a small palette) and the C6 gets no new buffers and no LVGL objects.
//
// Frame timing follows splash.cpp: elapsed time is derived from millis() and a
// frame is only pushed when the composed result actually changed.

#define GRID  SPLASH_GRID

// ─── Palette ─────────────────────────────────────────────────────────────────
// Cell values are indices into `pal[]`, and the C6's render path finds the
// dirty rectangle by diffing cell *values* between frames — so an index must
// keep its colour for as long as the index is reused. The table is therefore
// content-addressed and built once per scene/mood (pal_build), then never
// reordered: two colours can never trade indices, and a new colour is only ever
// appended. Every colour a scene can draw is pre-registered, so the mapping is
// constant for the whole scene and the diff stays exact.
#define PAL_BG  0            // pal[0] is the black stage
#define PAL_MAX SPLASH_PALETTE_MAX

static uint8_t  cells[GRID * GRID];   // 3.6 KB of static RAM, same as splash.cpp's
static uint16_t pal[PAL_MAX];
static uint8_t  pal_count;

// Brand + UI colours (spec §2.3 / the prototype's grid legends).
#define COL_MARK      0xF1ECECu   // 'O' / 'C' — the mark and the wordmark face
#define COL_WORD      0xB7B1B1u   // 'B' — wordmark body
#define COL_INNER     0x4B4646u   // 'i' / 'A' — mark interior, wordmark cut-outs
#define COL_INNER_ALT 0x5A5858u   // assemble "breathe" — the interior lifts
#define COL_LIMITED   0xE06C75u   // spec error red — limited blink

// ─── Module state ────────────────────────────────────────────────────────────
enum { SCENE_TYPEON, SCENE_ASSEMBLE, SCENE_SCANNER, SCENE_COUNT };

static bool      active = false;
static oc_mood_t mood   = OC_MOOD_IDLE;
static uint8_t   scene  = SCENE_TYPEON;
static bool      manual = false;    // a PWR scene override is in force
static bool      once   = false;    // one-shot assemble → back to the auto scene
static uint32_t  scene_ms = 0;      // scene intro, per the prototype's scene.time
static uint32_t  next_ms  = 0;      // earliest time the frame can change again
static uint32_t  frame_key = 0;     // identity of the frame last pushed
static bool      drawn = false;

// ─── Colour maths ────────────────────────────────────────────────────────────
// Same 5/6/5 truncation as LVGL's lv_color_to_u16(), i.e. the byte order
// splash_animations.h palettes are written in.
static inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

static inline uint16_t hex565(uint32_t rgb) {
    return rgb565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// An rgba blend over the black stage, pre-computed: `pct` is the alpha in
// percent, so the whole frame is a palette of opaque RGB565 colours.
static inline uint16_t shade(uint32_t rgb, int pct) {
    return rgb565(((rgb >> 16) & 0xFF) * pct / 100,
                  ((rgb >>  8) & 0xFF) * pct / 100,
                  ( (rgb      ) & 0xFF) * pct / 100);
}

// The prototype's adjustBrightness(base, 1.15), floored per channel.
static inline uint32_t brighten(uint32_t rgb) {
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t v = (rgb >> (16 - 8 * i)) & 0xFF;
        v = v * 115 / 100;
        if (v > 255) v = 255;
        out |= v << (16 - 8 * i);
    }
    return out;
}

static void pal_reset(void) {
    pal[PAL_BG] = 0x0000;
    pal_count   = PAL_BG + 1;
}

// Intern a colour: same colour → same index, new colour → a new index at the
// end. Never reorders, never reassigns.
static uint8_t pal_get(uint16_t color) {
    for (uint8_t i = 0; i < pal_count; i++)
        if (pal[i] == color) return i;
    if (pal_count >= PAL_MAX) return PAL_BG;   // pal_build() gets there first
    pal[pal_count] = color;
    return pal_count++;
}

static inline void put(int x, int y, uint16_t color) {
    if (x < 0 || x >= GRID || y < 0 || y >= GRID) return;
    cells[y * GRID + x] = pal_get(color);
}

// ─── Source art (official OpenCode material, research §2.3) ─────────────────
static const char *const OC_MARK[5] = {
    "OOOO",
    "O..O",
    "OiiO",
    "OiiO",
    "OOOO",
};

#define WORDMARK_H 7
static const char *const OC_WORDMARK[WORDMARK_H] = {
    ".................................C.....",
    "BBBB.BBBB.BBBB.BBB..CCCC.CCCC.CCCC.CCCC",
    "B..B.B..B.B..B.B..B.C....C..C.C..C.C..C",
    "BAAB.BAAB.BBBB.BAAB.CAAA.CAAC.CAAC.CCCC",
    "BAAB.BAAB.BAAA.BAAB.CAAA.CAAC.CAAC.CAAA",
    "BBBB.BBBB.BBBB.BAAB.CCCC.CCCC.CCCC.CCCC",
    ".....B.................................",
};

// Grid legend → colour; 0 means "leave the cell black".
static uint16_t grid_color(char ch) {
    switch (ch) {
        case 'O': case 'C': return hex565(COL_MARK);
        case 'B':         return hex565(COL_WORD);
        case 'i': case 'A': return hex565(COL_INNER);
        default:          return 0;
    }
}

// ─── Moods ───────────────────────────────────────────────────────────────────
static const uint32_t MOOD_BASE[OC_MOOD_LIMITED + 1] = {
    0xFAB283u,   // idle    — peach
    0xFAB283u,   // active
    0xFAB283u,   // busy
    0xF5A742u,   // near    — amber
    0xE06C75u,   // limited — red
};
// Only "busy" lifts the scanner head; the others reuse their base.
static const uint32_t MOOD_HEAD[OC_MOOD_LIMITED + 1] = {
    0xFAB283u, 0xFAB283u, 0xFFC09Fu, 0xF5A742u, 0xE06C75u,
};

static inline uint8_t auto_scene(void) {
    return (mood == OC_MOOD_IDLE) ? SCENE_TYPEON : SCENE_SCANNER;
}

// ─── Scene: typeon ───────────────────────────────────────────────────────────
// The wordmark is typed out letter by letter with a blinking peach cursor. The
// cursor is mood-coloured, so this scene also reports the mood.
#define TYPEON_LEAD_MS   200
#define TYPEON_CURSOR_MS 500
static const uint16_t TYPEON_DELAY[8] = { 60, 60, 60, 150, 60, 60, 250, 60 };
static const uint8_t  TYPEON_COL[8]   = {  0,  5, 10, 15, 20, 25, 30, 35 };

static uint32_t compose_typeon(uint32_t elapsed, uint32_t key) {
    if (elapsed < TYPEON_LEAD_MS) return key;   // still black
    const uint32_t t = elapsed - TYPEON_LEAD_MS;

    uint8_t drawn_letters = 0;
    uint32_t delay = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (delay <= t) drawn_letters = i + 1;
        delay += TYPEON_DELAY[i];
    }

    for (uint8_t i = 0; i < drawn_letters; i++) {
        const int lx = TYPEON_COL[i];
        for (int r = 0; r < WORDMARK_H; r++) {
            for (int c = 0; c < 4; c++) {
                const uint16_t col = grid_color(OC_WORDMARK[r][lx + c]);
                if (col) put(10 + lx + c, 26 + r, col);
            }
        }
    }

    const uint32_t phase = (t / TYPEON_CURSOR_MS) % 2;
    if (phase == 0) {
        const int cx = 10 + (drawn_letters ? TYPEON_COL[drawn_letters - 1] + 4 : 0);
        const uint16_t col = shade(MOOD_BASE[mood], 100);
        for (int r = 27; r <= 31; r++) put(cx, r, col);
    }

    // 0 while the lead-in is still black, 1 + (letters, phase) afterwards.
    return key | ((1u + drawn_letters * 2u + phase) << 8);
}

// ─── Scene: assemble ─────────────────────────────────────────────────────────
// The mark pops in cell by cell, clockwise, then the interior breathes.
#define ASM_SCALE     6
#define ASM_X         18
#define ASM_Y         15
#define ASM_STEP_MS   40
#define ASM_HOLD_MS   120
#define ASM_ALT_MS    800
// One-shot length: ring intro + two full breathe periods.
#define ASM_ONCE_MS   ((40 * 14) + ASM_HOLD_MS + 2 * ASM_ALT_MS)

static const uint8_t ASM_RING[14][2] = {   // top, right, bottom, left
    {0,0},{1,0},{2,0},{3,0},
    {3,1},{3,2},{3,3},{3,4},
    {2,4},{1,4},{0,4},
    {0,3},{0,2},{0,1},
};
// The mark's 2x2 interior, shared by the assemble scene and the limited blink.
static const uint8_t MARK_INNER[4][2] = { {1,2},{2,2},{1,3},{2,3} };

// One mark cell blown up to ASM_SCALE square.
static void asm_block(const uint8_t cell[2], uint16_t color) {
    const uint8_t idx = pal_get(color);
    for (int sr = 0; sr < ASM_SCALE; sr++) {
        const int y = ASM_Y + cell[1] * ASM_SCALE + sr;
        if (y < 0 || y >= GRID) continue;
        for (int sc = 0; sc < ASM_SCALE; sc++) {
            const int x = ASM_X + cell[0] * ASM_SCALE + sc;
            if (x >= 0 && x < GRID) cells[y * GRID + x] = idx;
        }
    }
}

static uint32_t compose_assemble(uint32_t elapsed, uint32_t key) {
    const uint32_t intro = ASM_STEP_MS * 14;   // 560 ms of ring cells
    if (elapsed < intro) {
        const int upto = (int)(elapsed / ASM_STEP_MS);      // 0..13
        for (int i = 0; i <= upto; i++)
            asm_block(ASM_RING[i], hex565(COL_MARK));
        return key | ((uint32_t)(upto + 1) << 8);
    }

    for (int i = 0; i < 14; i++) asm_block(ASM_RING[i], hex565(COL_MARK));

    const uint32_t t = elapsed - intro;
    uint16_t inner = hex565(COL_INNER);
    if (t >= ASM_HOLD_MS && ((t - ASM_HOLD_MS) / ASM_ALT_MS) % 2)
        inner = hex565(COL_INNER_ALT);
    for (int i = 0; i < 4; i++) asm_block(MARK_INNER[i], inner);

    return key | (inner == hex565(COL_INNER) ? 100u : 101u) << 8;
}

// ─── Scene: scanner ──────────────────────────────────────────────────────────
// The mark, plus an 8-block terminal scanner running underneath it. 54 frames
// of 40 ms (30 of 20 ms when busy): sweep out, hold, sweep back, then a long
// rest that fades the unlit blocks from 60% to 18% alpha.
#define SCAN_FRAMES      54
#define SCAN_FRAME_MS    40
#define SCAN_BUSY_FRAME_MS 20
#define SCAN_BUSY_FRAMES 30
#define SCAN_FADE_FROM   30          // frames 0..29 keep the unlit blocks at 60%
#define SCAN_HEAD_FRAME  17          // head is the range's right end before this
#define SCAN_LIMITED_FRAME 30        // "limited" freezes here, all blocks dim
#define SCAN_BLINK_MS    500

// Per-frame lit block range [lo, hi], inclusive; 0xFF = nothing lit.
//   0-13  sweep right,  14-16 rest,  17-29 sweep left,
//   30-53 rest, dimming (the prototype's SCANNER_LIT_RANGES).
static const uint8_t SCAN_LIT[SCAN_FRAMES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x16, 0x27,   //  0- 7
    0x27, 0x37, 0x47, 0x57, 0x67, 0x77,               //  8-13
    0xFF, 0xFF, 0xFF,                               // 14-16
    0x67, 0x57, 0x47, 0x37, 0x27, 0x16, 0x05,        // 17-23
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00,               // 24-29
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // 30-37
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // 38-45
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // 46-53
};

// Trail alpha per distance from the head: 0.65^(n-1) (index 0 is the head and
// index 1 is the brightened block), rounded to percent.
static const uint8_t SCAN_TRAIL[8] = { 100, 90, 65, 42, 27, 18, 12, 8 };

#define SCAN_MARK_X  22
#define SCAN_MARK_Y  10
#define SCAN_MARK_SCALE 4
#define SCAN_STRIP_X 14
#define SCAN_STRIP_Y 38
#define SCAN_BLOCK   3
#define SCAN_GAP     1

// Unlit alpha: 60% during the sweep, fading to 18% over the rest (1.0 → 0.3 of
// the prototype's fade, times its 0.6 base).
static int scan_unlit_pct(uint8_t frame) {
    if (frame < SCAN_FADE_FROM) return 60;
    const uint32_t d = (uint32_t)(frame - SCAN_FADE_FROM);
    return 60 - (int)((42 * d + 11) / 23);
}

static void scan_block(int x, uint8_t idx) {
    for (int r = 0; r < SCAN_BLOCK; r++) {
        const int dy = SCAN_STRIP_Y + r;
        for (int c = 0; c < SCAN_BLOCK; c++) {
            const int dx = SCAN_STRIP_X + x + c;
            if (dx >= 0 && dx < GRID && dy < GRID) cells[dy * GRID + dx] = idx;
        }
    }
}

static void scan_mark_cell(int mc, int mr, uint16_t color) {
    const uint8_t idx = pal_get(color);
    for (int sr = 0; sr < SCAN_MARK_SCALE; sr++) {
        const int dy = SCAN_MARK_Y + mr * SCAN_MARK_SCALE + sr;
        for (int sc = 0; sc < SCAN_MARK_SCALE; sc++) {
            const int dx = SCAN_MARK_X + mc * SCAN_MARK_SCALE + sc;
            if (dx >= 0 && dx < GRID && dy < GRID) cells[dy * GRID + dx] = idx;
        }
    }
}

static uint32_t compose_scanner(uint32_t elapsed, uint32_t key) {
    const bool limited = (mood == OC_MOOD_LIMITED);
    const bool busy    = (mood == OC_MOOD_BUSY);
    const uint8_t frame = limited ? SCAN_LIMITED_FRAME
                                  : (uint8_t)((elapsed /
                                       (busy ? SCAN_BUSY_FRAME_MS : SCAN_FRAME_MS)) %
                                      (busy ? SCAN_BUSY_FRAMES : SCAN_FRAMES));

    for (int mr = 0; mr < 5; mr++) {
        for (int mc = 0; mc < 4; mc++) {
            const uint16_t col = grid_color(OC_MARK[mr][mc]);
            if (col) scan_mark_cell(mc, mr, col);
        }
    }

    const uint8_t lit = SCAN_LIT[frame];
    const int lo = lit >> 4;
    const int hi = lit & 0x0F;
    const int head = (frame < SCAN_HEAD_FRAME) ? hi : lo;
    const int unlit = scan_unlit_pct(frame);
    for (int cell = 0; cell < 8; cell++) {
        uint32_t rgb = MOOD_BASE[mood];
        int alpha = unlit;
        if (lit != 0xFF && cell >= lo && cell <= hi) {
            const int trail = (cell > head) ? cell - head : head - cell;
            if (trail == 0) {
                rgb   = MOOD_HEAD[mood];
                alpha = 100;
            } else if (trail == 1) {
                rgb   = brighten(MOOD_BASE[mood]);
                alpha = 90;
            } else {
                alpha = SCAN_TRAIL[trail];
            }
        }
        scan_block(cell * (SCAN_BLOCK + SCAN_GAP), pal_get(shade(rgb, alpha)));
    }

    // At the limit the scanner sits dark and the mark's interior blinks red.
    uint32_t blink = 0;
    if (limited) {
        blink = (elapsed / SCAN_BLINK_MS) % 2;
        const uint16_t col = blink ? hex565(COL_LIMITED) : hex565(COL_INNER);
        for (int i = 0; i < 4; i++) scan_mark_cell(MARK_INNER[i][0], MARK_INNER[i][1], col);
    }

    return key | ((uint32_t)frame | (blink << 8)) << 8;
}

// ─── Palette contents ────────────────────────────────────────────────────────
// Pre-register every colour the current scene can draw, so the index→colour
// mapping stays constant while it runs and the C6's value-diff can never miss a
// changed pixel. Slot budget, worst case 30 of SPLASH_PALETTE_MAX — the scanner
// under "limited": black, two mark colours, the limited red, the head, the
// brightest trail step, the four tail steps (three of which coincide with ramp
// values) and the 24-step unlit ramp. Typeon needs 5, assemble 4.
static void pal_build(uint8_t s) {
    pal_reset();
    if (s == SCENE_TYPEON) {
        pal_get(hex565(COL_WORD));
        pal_get(hex565(COL_INNER));
        pal_get(hex565(COL_MARK));
        pal_get(shade(MOOD_BASE[mood], 100));            // the typing cursor
    } else if (s == SCENE_ASSEMBLE) {
        pal_get(hex565(COL_MARK));                       // ring
        pal_get(hex565(COL_INNER));                      // interior
        pal_get(hex565(COL_INNER_ALT));                  // interior, lifted
    } else {
        pal_get(hex565(COL_MARK));                       // mark ring
        pal_get(hex565(COL_INNER));                      // mark interior / blink
        pal_get(hex565(COL_LIMITED));                    // limited blink
        pal_get(shade(MOOD_HEAD[mood], 100));            // scanner head
        pal_get(shade(brighten(MOOD_BASE[mood]), 90));    // brightest trail step
        for (int t = 2; t <= 5; t++)                     // tail steps, 0.65^(t-1)
            pal_get(shade(MOOD_BASE[mood], SCAN_TRAIL[t]));
        for (int f = SCAN_FADE_FROM; f < SCAN_FRAMES; f++)   // the unlit ramp
            pal_get(shade(MOOD_BASE[mood], scan_unlit_pct((uint8_t)f)));
    }
}

// ─── Composition ─────────────────────────────────────────────────────────────
// Compose the whole stage and return a key that fully identifies the result:
// a tick whose key matches the last one pushed would paint an identical frame,
// so it is skipped.
static uint32_t compose(uint32_t elapsed) {
    memset(cells, 0, sizeof(cells));
    uint32_t key = (uint32_t)scene | ((uint32_t)mood << 4);
    switch (scene) {
        case SCENE_TYPEON:   return compose_typeon(elapsed, key);
        case SCENE_ASSEMBLE: return compose_assemble(elapsed, key);
        default:             return compose_scanner(elapsed, key);
    }
}

static void scene_set(uint8_t s, uint32_t now) {
    scene    = s;
    scene_ms = now;
    next_ms  = now;      // compose the first frame on the next tick
    drawn    = false;
    pal_build(s);        // a new scene may need new colours
}

// The instant at which the composed frame next changes — the event that drives
// the scene: the next letter landing, the cursor flipping, the next ring cell,
// the next scanner frame. Absolute ms, derived from the same constants the
// composers use. This is the frame-hold gate: a main loop runs far faster than
// the animation, so without it the 60×60 stage would be recomposed every pass.
static uint32_t next_change(uint32_t now) {
    const uint32_t e = now - scene_ms;
    switch (scene) {
        case SCENE_TYPEON: {
            if (e < TYPEON_LEAD_MS) return scene_ms + TYPEON_LEAD_MS;
            const uint32_t t = e - TYPEON_LEAD_MS;
            // The cursor flips every TYPEON_CURSOR_MS...
            uint32_t at = TYPEON_LEAD_MS + (t / TYPEON_CURSOR_MS + 1) * TYPEON_CURSOR_MS;
            // ...and letter k lands at the sum of the delays before it.
            uint32_t delay = 0;
            for (uint8_t i = 0; i < 8; i++) {
                if (delay > t) { if (TYPEON_LEAD_MS + delay < at) at = TYPEON_LEAD_MS + delay; break; }
                delay += TYPEON_DELAY[i];
            }
            return scene_ms + at;
        }
        case SCENE_ASSEMBLE: {
            const uint32_t intro = ASM_STEP_MS * 14;      // ring pop-in
            if (e < intro) return scene_ms + (e / ASM_STEP_MS + 1) * ASM_STEP_MS;
            const uint32_t t = e - intro;                 // then the interior breathes
            if (t < ASM_HOLD_MS) return scene_ms + intro + ASM_HOLD_MS;
            return scene_ms + intro + ASM_HOLD_MS +
                   ((t - ASM_HOLD_MS) / ASM_ALT_MS + 1) * ASM_ALT_MS;
        }
        default: {
            if (mood == OC_MOOD_LIMITED)                  // strip frozen; only blink
                return scene_ms + (e / SCAN_BLINK_MS + 1) * SCAN_BLINK_MS;
            const uint32_t step = (mood == OC_MOOD_BUSY) ? SCAN_BUSY_FRAME_MS : SCAN_FRAME_MS;
            return scene_ms + (e / step + 1) * step;
        }
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────
void oc_splash_start(void) {
    splash_set_external(true);
    active = true;
    manual = false;
    once   = false;
    scene_set(auto_scene(), millis());
}

void oc_splash_stop(void) {
    active = false;
    drawn  = false;
    splash_set_external(false);
}

void oc_splash_tick(void) {
    if (!active) return;
    const uint32_t now = millis();
    if (drawn && now < next_ms) return;
    if (once && now - scene_ms >= ASM_ONCE_MS) {
        once   = false;
        manual = false;
        scene_set(auto_scene(), now);
    }

    const uint32_t key = compose(now - scene_ms);
    next_ms = next_change(now);
    if (drawn && key == frame_key) return;   // nothing new on screen
    frame_key = key;
    drawn     = true;
    splash_render_external(cells, pal);
}

void oc_splash_set_mood(oc_mood_t m) {
    if ((int)m < (int)OC_MOOD_IDLE || (int)m > (int)OC_MOOD_LIMITED) return;
    // A payload arrives about once a minute and usually repeats the mood; the
    // scene must not restart for a value it is already showing.
    if (m == mood) return;
    mood = m;
    if (!active) return;                // not on screen: oc_splash_start() picks up

    if (!manual && auto_scene() != scene) {
        scene_set(auto_scene(), millis());       // the mood's own scene is a different one
    } else {
        // Same scene, or a manual override the prototype also keeps: the
        // timeline runs on and only the tint changes, so the wordmark isn't
        // retyped and the scanner doesn't jump back to frame 0. The palette is
        // rebuilt for the new mood, which splash_render_external() sees as a
        // remap and answers with a full repaint.
        pal_build(scene);
        drawn = false;
    }
}

void oc_splash_next_scene(void) {
    if (!active) return;
    manual = true;
    once   = false;
    scene_set((uint8_t)((scene + 1) % SCENE_COUNT), millis());
}

void oc_splash_play_assemble(void) {
    if (!active) return;
    manual = false;
    once   = true;
    scene_set(SCENE_ASSEMBLE, millis());
}

bool oc_splash_is_active(void) {
    return active;
}
