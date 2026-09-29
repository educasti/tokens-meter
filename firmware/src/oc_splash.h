#pragma once
#include <stdint.h>

// OpenCode splash — the same shared 60x60 cell canvas Clawd uses, redrawn from
// official OpenCode material (the mark, the wordmark, the terminal block
// scanner and its typing cursor). This module owns no LVGL objects and no
// pixel buffer: it composes a cell buffer plus a small RGB565 palette and
// hands both to splash_render_external() (splash.h), which feeds the same
// render path as the Clawd animations — LVGL canvas on PSRAM boards, direct
// panel blit on the C6.
//
// Ported from the approved prototype design/opencode-screen/oc-splash.js
// (scenes, timings, colours and the scanner frame table are identical).
//
// Lifecycle: the owner shows the splash container (splash_show()), then calls
// oc_splash_start(); oc_splash_tick() runs from the main loop and
// oc_splash_stop() hands the canvas back to Clawd.
enum oc_mood_t {
    OC_MOOD_IDLE,     // no sessions        → typeon
    OC_MOOD_ACTIVE,   // a >= 1             → scanner
    OC_MOOD_BUSY,     // a >= 2             → scanner, 20 ms/frame
    OC_MOOD_NEAR,     // 5h/week >= 75%     → scanner, amber
    OC_MOOD_LIMITED,  // limit reached      → scanner frozen, red blink
};

// Take over the shared splash canvas (splash_set_external(true)) and restart
// the scene intro.
void oc_splash_start(void);

// Hand the canvas back to Clawd (splash_set_external(false)).
void oc_splash_stop(void);

// Compose the next frame if it changed. Call every loop; no-op unless started.
void oc_splash_tick(void);

// Auto scene: OC_MOOD_IDLE → typeon, every other mood → scanner. A manual
// scene (set by oc_splash_next_scene) is left alone.
void oc_splash_set_mood(oc_mood_t m);

// PWR on the OpenCode splash: typeon → assemble → scanner → typeon, and the
// choice sticks until the mood changes.
void oc_splash_next_scene(void);

// One-shot assemble (a usage window just reset), then back to the auto scene.
void oc_splash_play_assemble(void);

bool oc_splash_is_active(void);
