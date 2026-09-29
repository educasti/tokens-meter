# WP4 — OpenCode data model + usage screen (LVGL)

Repo: /Users/educasti/Projects/Personal/tokens-meter (branch feat/opencode-screens). Read first: `design/opencode-screen/IMPL.md` (WP4 contract + repo rules), `design/opencode-screen/SPEC.md` §3, §4, §7, §8 and **Annex A (exact coordinates)**, the approved prototype `design/opencode-screen/prototype.html` (function `renderOpenCode` = the visual reference), `firmware/src/ui.cpp` (how panels/labels/bars are built: `make_panel`, `make_bar`, fonts, `compute_layout`, `board_caps()`), `firmware/src/main.cpp` `parse_json` (ArduinoJson 7 usage), `firmware/src/theme.h`.

Edit/create ONLY: `firmware/src/oc_data.h`, `firmware/src/oc_data.cpp`, `firmware/src/ui_opencode.h`, `firmware/src/ui_opencode.cpp` (all new).

Fonts/images come from WP1 (being generated in parallel): use `LV_FONT_DECLARE(font_plex_48)` … `font_plex_12` and `#include "oc_logo.h"` (`oc_mark_l/m/s`, `oc_wordmark_l/m/s`, RGB565 lv_image_dsc_t). If they don't exist yet when you compile, wait/retry later — do not create them yourself.

Implement exactly the contract. Visual rules:
- Own colour defines (OpenCode tokens from SPEC §3) local to ui_opencode.cpp; do not edit theme.h.
- Panels: `lv_obj` with 1 px `#3c3c3c` border, radius 0, transparent bg, no padding/scroll; children absolutely positioned per Annex A. Chip = label with bg `#1e1e1e`, padding-x, fixed height. Segmented bar = N small `lv_obj` rects (34/25/18) — create them once, only change colours on update. Stats (L/M only), status line with a blinking square (`#7fd88f`, 1 Hz via `oc_usage_tick`).
- Texts/format: `Resets in 2h 14m` / `6d 13h` / `42m`, `No active window` when r5 == -1, chip `5h`/`week` (+` · est.` when src est; `limit` when limited and that pct ≥ 100), red `#e06c75` chip+bar when pct ≥ 85 or limited; stats `7d tokens` / `top model` / `month` (`3% · 14d`) or `7d cost` in src none; tokens `812k`/`3.2M`/`107.0M`/`1.2B`; money `$1.10`; model truncated to 12 (L) / 9 (M) chars + `…`; status `build · working`, `idle · last activity 21m ago`, `○ stale · updated 12m ago`, `○ bluetooth disconnected` (use `o` if WP1 reports no ○ glyph — check the generated font range).
- Stale (> 300000 ms since last update) or BLE off: panels + stats container at `LV_OPA_40`; header/status at full opacity.
- Keep object count small (C6 has no PSRAM): one container per panel, reuse objects.
- Verify it compiles in the sim (it isn't wired into ui.cpp yet — WP5 does that): `pio run -d firmware -e sim`.

When done reply exactly: `DONE WP4` + one line summary.
