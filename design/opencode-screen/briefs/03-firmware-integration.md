# Brief 03 — Firmware + daemon integration points for a new screen (exploration only)

You are a research subagent. **Do not modify any code.** Your only output is the findings file below.

## Context
Repo: `/Users/educasti/Projects/Personal/tokens-meter` — "Clawdmeter", ESP32-S3/C6 firmware (PlatformIO, Arduino core 3, LVGL 9, NimBLE) for a desk display showing Claude Code usage, plus host daemons (Linux bash, macOS Python, Windows Python) that push a JSON payload over BLE GATT. Read `CLAUDE.md` and `README.md` first — they document architecture, HAL, boards, gotchas. We plan to add a **new screen showing OpenCode usage** (tokens/cost per model, not rate-limit %), fed by the daemon reading OpenCode's local data. The user is on **macOS** (daemon: `daemon/claude_usage_daemon.py`).

## Questions to answer (cite file:line for everything)
1. **Screens & navigation**: list the screen enum and every screen that exists (`ui.h/ui.cpp`, `main.cpp`). How is each screen built (LVGL objects, containers, fonts), shown/hidden, and navigated (tap toggles splash↔usage; PWR short-press; BOOT; the bluetooth screen; `SCREEN_CONTROLLER`?). Exactly which code would need to change to insert a new screen into the rotation, per board button count (1-button LCD-4, 2-button 1.8, 3-button 2.16).
2. **Layout system**: how `compute_layout()` and `board_caps()` drive responsive sizes; the breakpoints; how the current usage screen lays out its bars, labels, countdowns, battery/BLE icons; margins for round corners. Sketch the current usage screen layout in ASCII with coordinates for 480×480 and 368×448.
3. **Data path**: `data.h` `UsageData`, JSON parsing in `ble.cpp` (ArduinoJson? buffer size, max payload length, MTU), freshness (`DATA_FRESH_MS`), `has_received_data`, refresh request. Could a second payload type (e.g. `{"t":"oc",...}`) be added on the same RX characteristic, or would a new characteristic be cleaner? What are the size constraints?
4. **Fonts & assets pipeline**: how fonts are generated/patched (`docs/fonts.md`), which fonts/sizes exist, flash budget per font; how icons/logos are converted (`tools/png_to_lvgl.js`, `logo.h`), RGB565 vs RGB565A8. Flash/partition usage headroom per board (check `platformio.ini` partitions; estimate current app size if a `.pio` build exists — do not run a full build unless it takes under ~3 min for one env; `pio run -d firmware -e sim` is acceptable if SDL2 is installed).
5. **Daemon side (macOS)**: structure of `daemon/claude_usage_daemon.py` — poll loop, BLE write, payload construction, config file, heartbeat. Where would an OpenCode collector plug in, and how would it send a second payload? Same for Linux bash and Windows daemons (brief).
6. **Simulator**: how `firmware/sim/scenario.jsonl` drives screens, so a new screen could be previewed in the sim.
7. **Risks**: C6 no-PSRAM memory limits, `#ifdef BOARD_*` ban, touch/tap handling conflicts, anything else a new screen must respect.

## Deliverable
Write **`/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/03-firmware-integration.md`** with one section per question, file:line references, ASCII layout sketches, and a final **"Recommended integration plan"** (bullet list: files to touch, navigation proposal per button count, payload strategy, font/asset budget).

When finished, reply with one line: `DONE 03` plus the file path.
