# WP3 — OpenCode splash on the shared splash canvas

Repo: /Users/educasti/Projects/Personal/tokens-meter (branch feat/opencode-screens). Read first: `design/opencode-screen/IMPL.md` (WP3 contract + repo rules), `design/opencode-screen/SPEC.md` §5, the APPROVED reference implementation `design/opencode-screen/oc-splash.js` (port its scenes, timings, colours and the scanner frame table exactly), `firmware/src/splash.h`, `firmware/src/splash.cpp` (both render paths: PSRAM canvas `render_frame` and the C6 strip `blit_cells`; `splash_tick`, `splash_show`), `firmware/src/splash_geometry.h`.

Edit/create ONLY: `firmware/src/splash.h`, `firmware/src/splash.cpp`, `firmware/src/oc_splash.h` (new), `firmware/src/oc_splash.cpp` (new).

Implement
- splash.cpp: `splash_set_external(bool)` and `splash_render_external(cells, palette)` as specified. External mode must pause Clawd playback/walks/picks without losing state; turning it off must force a full repaint (`force_full`/prev_valid) and resume Clawd. `splash_render_external` goes through the SAME render path for both PSRAM and non-PSRAM builds (reuse `render_frame`). Palette is indexed by cell value; allow up to 32 entries.
- oc_splash.cpp: compose a 60×60 `uint8_t` cell buffer (static, 3.6 KB) + a small palette each tick, only re-rendering when the frame actually changes (frame-hold timing with `millis()`, like splash.cpp). Scenes: typeon, assemble, scanner (+ mood variants busy/near/limited) exactly as oc-splash.js. Alpha blends are pre-computed RGB565 colours over black (use `lv_color_to_u16(lv_color_hex(...))` or a helper matching splash.cpp's palette format — check how splash_animations.h palettes are encoded and match byte order).
- No LVGL objects of its own; no new large buffers; no `#ifdef BOARD_*`.
- Verify it compiles: `pio run -d firmware -e sim` and `pio run -d firmware -e waveshare_amoled_216_c6` (non-PSRAM path). Nothing calls oc_splash yet — that's WP5 — but add a temporary local test only if you remove it afterwards.

When done reply exactly: `DONE WP3` + one line summary.
