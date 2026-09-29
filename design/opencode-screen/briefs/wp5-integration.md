# WP5 — Integration: screens, navigation, buttons, RX routing, simulator

Repo: /Users/educasti/Projects/Personal/tokens-meter (branch feat/opencode-screens). Read first:
- `design/opencode-screen/IMPL.md` (WP5 contract + repo rules — **no `#ifdef BOARD_*` in shared code**)
- `design/opencode-screen/SPEC.md` §5 (splash moods), §6 (navigation — the key section), §7, §8
- the finished modules you wire together (do NOT edit them): `firmware/src/oc_data.h`, `firmware/src/ui_opencode.h`, `firmware/src/oc_splash.h`, plus the new `splash_set_external` / `splash_render_external` in `firmware/src/splash.h`
- `firmware/src/ui.h`, `firmware/src/ui.cpp` (`ui_show_screen`, `global_click_cb`, `ui_toggle_splash`, `ui_tick_anim`, `apply_battery_visibility`, `prev_non_splash_screen`, `update_view_state`, BLE status), `firmware/src/main.cpp` (button block, PWR handler, RX block, `parse_json`), `firmware/src/ble.cpp` (`rx_buf`, `data_ready`, `ble_get_data`), `firmware/src/boards/sim/*` (ble_sim.cpp scenario playback, sim_platform.cpp keys + autoshot, board.h), `firmware/platformio.ini` `[env:sim]`.

Edit ONLY: `firmware/src/ui.h`, `firmware/src/ui.cpp`, `firmware/src/main.cpp`, `firmware/src/ble.cpp`, `firmware/src/boards/sim/*`, `firmware/platformio.ini`, `firmware/sim/scenario-opencode.jsonl` (new).

## Implement
1. **Screens** — `screen_t`: `SCREEN_SPLASH, SCREEN_USAGE, SCREEN_OC_SPLASH, SCREEN_OC_USAGE, SCREEN_COUNT`. `ui_init` calls `oc_usage_init(scr)` and attaches the same tap handler to `oc_usage_get_root()`. `ui_show_screen`:
   - SPLASH: `oc_splash_stop()`, `splash_show()` (Clawd).
   - OC_SPLASH: `splash_show()` then `oc_splash_start()` (shared canvas; no Clawd mascot/logo, battery hidden like the Clawd splash).
   - USAGE: as today. OC_USAGE: `oc_usage_show()`, battery visible, corner mascot/logo hidden, title label hidden.
   - Leaving OC_USAGE → `oc_usage_hide()`; leaving OC_SPLASH → `oc_splash_stop()`.
2. **Cycle** — `ui_next_screen()` / `ui_prev_screen()` over `[SPLASH, USAGE, OC_SPLASH, OC_USAGE]`, cyclic; while `!oc_has_data()` the cycle is only `[SPLASH, USAGE]`. Replace the splash↔usage toggle in `global_click_cb` with `ui_next_screen()` (keep `ui_toggle_splash` working or remove it if unused). Keep `prev_non_splash_screen` semantics only if still needed.
3. **Page indicator** — row of dots (one per screen in the current cycle: 4 or 2), 6 px circles, 8 px gap, centred horizontally, centre y = 472 (height ≥ 460), 440 (≥ 300), 234 (else); current `#eeeeee`, others `#484848`; shown on every screen change, hidden 1500 ms later (use an `lv_timer` or the tick). Must sit on top of everything (create last / `lv_obj_move_foreground`).
4. **`ui_update_opencode(const OcData*)`** (declare in ui.h): `oc_usage_update(d)`; mood = LIMITED if `d->limited`; NEAR if max(p5,pw) ≥ 75; BUSY if a ≥ 2; ACTIVE if a ≥ 1; else IDLE → `oc_splash_set_mood()`; if p5 or pw dropped by ≥ 5 vs the previous payload → `oc_splash_play_assemble()`.
5. **BLE status** — `ui_update_ble_status` also calls `oc_usage_set_ble(state == connected)` (use the same "connected" notion the usage screen uses). Call it once at init too.
6. **Ticks** — `ui_tick_anim()` must call `oc_usage_tick()` when on OC_USAGE (don't early-return before it); `main.cpp` loop calls `oc_splash_tick()` next to `splash_tick()`.
7. **Buttons (main.cpp)** — replace the press/release HID logic with tap-vs-hold (`HOLD_MS 300`):
   - PRIMARY: on press start a timer (wake-swallow unchanged: a swallowed press does nothing until release). Released before 300 ms → navigate: `ui_prev_screen()` if `board_caps().button_count >= 2`, else `ui_next_screen()`. Still held at 300 ms → `ble_keyboard_press(0x2C, 0)` (Space) and keep it down until release → `ble_keyboard_release()`.
   - SECONDARY (only if button_count ≥ 2): tap → `ui_next_screen()`; held ≥ 300 ms → `ble_keyboard_press(0x2B, 0x02)` until release.
   - Each navigation counts as activity (call `idle_note_activity()` or whatever idle.h exposes, consistent with the existing code).
   - PWR short: SPLASH → `splash_next()`; OC_SPLASH → `oc_splash_next_scene()`; else `brightness_cycle()`. Pair gesture untouched.
   - Update the comment block above the button code to describe the new mapping.
8. **RX routing (main.cpp)** — in the `ble_has_data()` block: `const char *msg = ble_get_data();` if `oc_is_payload(msg)` → `oc_parse` → `ui_update_opencode` + `ble_send_ack()` (nack on failure) and **skip** `parse_json`/usage_rate/chime/`ui_update` entirely; else the existing Claude path unchanged.
9. **ble.cpp** — 2-slot RX FIFO: `onWrite` pushes into the next free slot (drop the oldest if both are full); `ble_has_data()`/`ble_get_data()` pop in order. Keep truncation at 511 bytes and all the owner/encryption checks. The sim's `ble_sim.cpp` implements the same `ble.h`, keep it compatible.
10. **Simulator**
   - `boards/sim/board.h`: wrap `LCD_WIDTH`/`LCD_HEIGHT`/`BOARD_NAME` in `#ifndef` so envs can override them; update the key-map comment (b/n tap = prev/next, hold = HID).
   - `platformio.ini`: add `[env:sim_368]` and `[env:sim_240]` that `extends = env:sim` and append `-DLCD_WIDTH=368 -DLCD_HEIGHT=448` / `-DLCD_WIDTH=240 -DLCD_HEIGHT=240` (and a matching BOARD_NAME) via `build_flags = ${env:sim.build_flags} ...`. Make sure `boards/sim/caps.cpp` reports those sizes and a button_count of 2 (and `SIM_BUTTONS=1` env var → 1, to test single-button boards) — do the env var check in sim code only.
   - `SIM_START_SCREEN=splash|usage|oc_splash|oc_usage`: in sim code only, after the scenario has delivered at least one Claude and one OC payload (or after 1500 ms), call `ui_show_screen(...)` once.
   - `firmware/sim/scenario-opencode.jsonl`: interleave Claude lines (copy two from `sim/scenario.jsonl`) with OC lines for these states, each with a `name` and `hold_ms` 3000: real (0% / no active window / week 0% rw 9430 / month 3% rm 20354, a 0, la 1260, src api), activa (37/134, 22/5040, a 1, ag build), varias (58/71, 41/5040, a 2, ag plan, m kimi-k3 ms 61), cerca (91/42, 78/2880), limite (100/18, 83/2880, st limited, a 0, la 240), est (src est, 12/250, 8/9430, a 1), consumo (src none, tk 3221, cd 0, c7 1.1). Use t7 106969–131700, m ds-v4.1-flash, ms 81–93, pm/rm as in the prototype. Compact JSON, ≤ 240 bytes each.
11. **Verify** — all must build: `PLATFORMIO_BUILD_DIR=/tmp/pio-wp5 pio run -d firmware -e sim -e sim_368 -e sim_240`, then (sequentially, same prefix but a different dir per env is fine) `-e waveshare_amoled_216 -e waveshare_amoled_216_c6 -e waveshare_amoled_18 -e waveshare_amoled_18_c6 -e waveshare_amoled_206 -e waveshare_lcd_154 -e waveshare_lcd_4`. Report the flash usage line of `waveshare_amoled_216`. Take headless sim screenshots to self-check: `cd firmware && SDL_VIDEODRIVER=dummy SIM_SCENARIO=sim/scenario-opencode.jsonl SIM_START_SCREEN=oc_usage SIM_AUTOSHOT_MS=2500 SIM_AUTOSHOT_PATH=/tmp/wp5-oc.bmp /tmp/pio-wp5/sim/program` (adjust the program path to your build dir).

Do not commit. Reply exactly `DONE WP5` + the 2.16 flash line.
