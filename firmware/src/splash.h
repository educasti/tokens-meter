#pragma once
#include <stdint.h>
#include <lvgl.h>

// Initialize splash module. Creates the canvas widget inside `parent` and
// allocates the 480x480 pixel buffer (PSRAM).
void splash_init(lv_obj_t *parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// Mini animated creature for embedding elsewhere (e.g. the idle screen).
// Renders the named official animation (e.g. "cloud") at ~px×px
// inside `parent`; returns the canvas object (position it with lv_obj_align) or
// NULL if the animation isn't found / allocation fails. Drive it with
// splash_mini_tick(). One mini creature at a time.
lv_obj_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px);
void splash_mini_tick(void);

// Corner mascot (usage screen, PSRAM boards): the still Clawd idles in the
// logo slot, does occasional acts, and takes walk-off/lurk/walk-back trips.
// feet_y = px of the art's ground line; cell = px per art cell in the corner.
lv_obj_t* splash_mascot_create(lv_obj_t *parent, int slot_x, int feet_y, int cell);
void splash_mascot_tick(void);
void splash_mascot_set_visible(bool v);

// ─── External canvas owner (see oc_splash.h) ────────────────────────────────
// Hand the shared splash canvas to another module. While external mode is on,
// splash_tick() stops advancing Clawd: playback, the walk choreography and the
// rate-driven picks are frozen where they stand (nothing is reset, so Clawd
// resumes mid-pose), splash_show() no longer picks, and splash_mascot_tick()
// idles. Turning it off forces a full repaint and resumes Clawd.

// Palette index cap for splash_render_external() — cell values are 0-based
// indices into `palette`, and a 60x60 frame never needs more than 32 of them.
// (The generated Clawd animations use the first 16.)
#define SPLASH_PALETTE_MAX 32

void splash_set_external(bool on);

// Paint one frame from another module's cell buffer. `cells` holds 60*60
// palette indices; `palette` maps an index to an RGB565 colour (index 0 is the
// background). Both are read during the call only, so the caller may compose
// them in place and skip re-painting when nothing changed. Goes through the
// same render path as the Clawd animations on both PSRAM and non-PSRAM
// boards.
void splash_render_external(const uint8_t *cells, const uint16_t *palette);
