# 03 — Firmware + daemon integration points for an OpenCode screen

Scope: exploration only, no repo code touched. All paths relative to repo root; `file:line` refs are from the working tree at commit `f1696b2`.

**Headline findings (things the brief / CLAUDE.md don't tell you):**

1. **There are only two screens today** — `SCREEN_SPLASH` and `SCREEN_USAGE` (`firmware/src/ui.h:5-9`). The "Bluetooth screen" and `SCREEN_CONTROLLER` mentioned in `CLAUDE.md` no longer exist in code (the `bt_*` layout fields in `ui.cpp:60-67` are leftovers, and `main.cpp:345-348` comments "single non-splash view, no more screens"). The "usage" screen internally has three *sub-views* (pair / idle / live), not separate screens.
2. **Key collision:** the existing payload already uses `"t"` for the wall-clock epoch (`main.cpp:120`), so the brief's suggested `{"t":"oc",...}` discriminator would be misparsed. Use a different key (e.g. `"k":"oc"`).
3. **Unknown payloads today = "no data" idle screen.** `parse_json` defaults `ok` to `false` (`main.cpp:122`); any payload lacking `"ok":true` flips `data_ok=false` and sends the usage view to the idle "Zzz" sub-view (`ui.cpp:595-596`, `687-690`). An OpenCode payload must be routed *before* `parse_json`, or it will blank the Claude screen.
4. **The macOS (and Windows) Python daemons have no heartbeat and no `poll_interval` config** — that is Linux-bash only. macOS polls every hard-coded `POLL_INTERVAL = 60` (`daemon/claude_usage_daemon.py:32`), which is why 90 s `DATA_FRESH_MS` never lapses there.
5. `button_count` is **1** on the 1.8 S3/C6, 2.06 and LCD-4, **2** on 2.16 S3, 2.16 C6, LCD-1.54 (`boards/*/caps.cpp`, `BOARD_HAS_SECONDARY_BUTTON` in each `board.h`). The PWR button is separate from `button_count` (owned by `power_hal`), and is a no-op on LCD-4.

---

## 1. Screens & navigation

### 1.1 Screen inventory

| Screen | Enum | Built in | Notes |
|---|---|---|---|
| Splash | `SCREEN_SPLASH` (`ui.h:5`) | `splash_init(scr)` `ui.cpp:560`; `splash.cpp` | Pixel-art canvas, own root container (`splash_get_root()`), click handler attached `ui.cpp:562-564` |
| Usage | `SCREEN_USAGE` (`ui.h:6`) | `init_usage_screen()` `ui.cpp:472-541` | `usage_container` full-size transparent `lv_obj` |
| — | `SCREEN_COUNT` (`ui.h:7`) | — | Only referenced by the enum; not used for iteration anywhere |

Everything hangs off **one** LVGL screen, `lv_screen_active()` (`ui.cpp:548`); "screens" are sibling full-size containers toggled with `LV_OBJ_FLAG_HIDDEN`. There is no `lv_screen_load()`.

### 1.2 Usage screen construction (`ui.cpp:472-541`)

```
scr (lv_screen_active, black bg)
├─ usage_container  (L.scr_w × L.scr_h, click → global_click_cb)         ui.cpp:473-480
│   ├─ lbl_title    (title_font; "Usage" or daemon clock HH:MM)          ui.cpp:482-488
│   ├─ usage_group  (full-size, transparent; live view = v2)             ui.cpp:492-499
│   │   ├─ panel_session  make_usage_panel(...)  y = content_y           ui.cpp:501-503
│   │   └─ panel_weekly   make_usage_panel(...)  y = content_y+panel_h+gap  ui.cpp:525-528
│   ├─ pair_group   (v0, "To pair / hold the power button …")            ui.cpp:419-448
│   ├─ idle_group   (v1, mini "cloud" creature)                          ui.cpp:453-470
│   └─ lbl_anim     (bottom-mid status line, mono font, spinner)         ui.cpp:536-540
├─ (splash root, created by splash_init)
├─ corner mascot (PSRAM) or logo_img (C6)                                ui.cpp:566-580
└─ battery_img (top-right, deleted if !has_battery)                      ui.cpp:582-590
```

Reusable builders: `make_panel` (`ui.cpp:317-332`, radius 8, `COL_PANEL`, `EVENT_BUBBLE`), `make_bar` (`334-347`), `make_pill` (`358-371`), `make_usage_panel` (`391-415`: pct label + pill + bar + reset label). Palette in `theme.h` (bg `#000`, panel `#1f1f1e`, text `#faf9f5`, dim `#b0aea5`, accent/amber `#d97757`, green `#788c5d`, red `#c0392b`, bar-bg `#2a2a28`).

Sub-view selection (`update_view_state`, `ui.cpp:682-699`): BLE down → pair (0); connected and fresh valid data → usage (2); else idle (1). Re-run every loop from `ui_tick_anim()` (`ui.cpp:701-704`) and on BLE state change (`ui.cpp:802-810`).

### 1.3 Show/hide + navigation

- `ui_show_screen()` (`ui.cpp:772-791`): hides `usage_container` + splash, then a `switch` shows one. Also toggles mascot/logo visibility and battery (`apply_battery_visibility` hides battery on splash only, `ui.cpp:760-764`). Tracks `prev_non_splash_screen` (`ui.cpp:759`, `788`).
- **Tap:** `global_click_cb` (`ui.cpp:766-770`) on `usage_container` and the splash root. Splash → `prev_non_splash_screen`; anything else → splash. Panels use `EVENT_BUBBLE` so a tap anywhere reaches the container. Touch reaches LVGL through `my_touch_cb` (`main.cpp:61-98`), including the wake-swallow logic. `ui_toggle_splash()` (`ui.cpp:793-796`) duplicates the same logic; it has no other caller.
- **Boot:** `ui_show_screen(SCREEN_SPLASH)` (`main.cpp:234`).
- **BOOT / PRIMARY:** *not* navigation — HID Space (Claude Code push-to-talk) (`main.cpp:313-325`).
- **SECONDARY** (2.16 S3/C6, LCD-1.54): HID Shift+Tab (`main.cpp:327-341`).
- **PWR short-press** (`power_hal_pwr_pressed()`, `main.cpp:343-350`): on splash → `splash_next()` (cycle animation); otherwise → `brightness_cycle()` (4 levels 64/128/200/255, `brightness.cpp:8`). This is the *only* PWR action, and the comment explicitly relies on there being one non-splash view.
- **PWR hold ~3 s + release:** hold-to-pair (`main.cpp:242-286`). Must not be disturbed.
- Idle/sleep: first press/touch while asleep is swallowed (`idle_consume_wake_press`, `main.cpp:344`, `318`, `333`, and `my_touch_cb`). Any new nav input must go through the same wake-swallow.

### 1.4 What must change to insert a screen

Minimum, board-independent:

1. `ui.h:5-8` — add `SCREEN_OPENCODE` before `SCREEN_COUNT`.
2. `ui.cpp` — new file-scope widget pointers, `L` fields + `compute_layout()` values for 3 breakpoints (`ui.cpp:27-68`, `75-179`), a `init_opencode_screen(scr)` called after `init_usage_screen` (`ui.cpp:559`), `ui_update_opencode(const OcData*)`.
3. `ui_show_screen()` `ui.cpp:776-780` — add a `case`; add the container to the initial `lv_obj_add_flag(..., HIDDEN)` list (`ui.cpp:773`).
4. `global_click_cb` / `ui_toggle_splash` (`ui.cpp:766-796`) — currently a **2-state** toggle (`prev_non_splash_screen`). Needs a defined rule (see plan).
5. `ui_tick_anim()` early-returns unless `current_screen == SCREEN_USAGE` (`ui.cpp:702`) — the OC screen needs its own freshness/tick or shares `update_view_state` logic. Also `apply_battery_visibility` (`ui.cpp:760`), corner mascot (`ui.cpp:782`) and `lbl_title` clock are usage-specific.
6. `main.cpp:343-350` — PWR handler: currently `else brightness_cycle()` for every non-splash screen. Adding a screen that PWR should page through changes this.
7. `main.cpp:374-396` — payload routing (see §3).

Per button count (navigation is where boards genuinely differ):

| Board class | Inputs available for screen switching | Constraint |
|---|---|---|
| **1-button, no PWR** — LCD-4 (`power.cpp:14-16` all stubs) | Touch only (tap). BOOT = PTT, must stay. | Only touch can page. Tap already means splash↔usage. |
| **1-button + PWR** — 1.8 S3, 1.8 C6, 2.06 | Touch + PWR short-press | PWR short is currently splash-anim / brightness. |
| **2-button + PWR** — 2.16 S3, 2.16 C6, LCD-1.54 | Touch + PWR (SECONDARY = Shift+Tab, must stay for Claude Code) | Same as above; SECONDARY is *not* free. |

Conclusion: **no board has a spare physical button.** Nav must be touch-first, with PWR as an optional accelerator on boards that have it. No `#ifdef BOARD_*` needed: `power_hal_pwr_pressed()` simply never fires on LCD-4, and shared code stays the same (rule 10 in `CLAUDE.md`).

---

## 2. Layout system

### 2.1 `compute_layout()` (`ui.cpp:75-179`)

Reads `board_caps()` (`hal/board_caps.h:11-21`: `width`, `height`, `button_count`, `has_rotation`, `has_battery`, `has_imu`). Sets shared defaults (`ui.cpp:78-104`) then **branches on height only**:

| Breakpoint | Condition | Target | `content_y` | panel_h | gap | bar_y / reset_y | fonts (title / pct / pill+reset / anim) |
|---|---|---|---|---|---|---|---|
| Large | `height >= 460` (`ui.cpp:106`) | 480×480 (2.16, LCD-4) | 100 | 150 | 16 | 56 / 94 | tiempos_56 / styrene_48 / styrene_28 / mono_32 |
| Compact | `height >= 300` (`120`) | 368×448 (1.8), also 410×502 (2.06) is ≥460 → Large | 85 | 130 | 12 | 48 / 78 | same as Large (title/pct/pill), bt fonts smaller |
| Small | else (`134`) | 240×240 (LCD-1.54) | 44 | 74 | 6 | 30 / 46 | tiempos_34 / styrene_24 / styrene_14 / mono_18; `margin=8`, `bar_h=12`, `small_icons=true` |

Common: `margin=20` (8 small), `title_y=30` (4 small), `bar_h=24` (12), `content_w = scr_w − 2·margin` (`ui.cpp:178`), `panel_pad 16/12` (10/6). Note the 2.06 (410×502) lands on **Large** with `content_w=370`, and the 1.8 (448 tall) on **Compact** — the breakpoint is height-only, so a new screen must also cope with widths of 240/328/370/440.

Corner elements: mascot at `x=margin`, slot `logo_y=title_y-10` (`ui.cpp:98`, `566-580`); battery icon at `x = scr_w − batt_w − margin`, `y = batt_y` (`ui.cpp:584`), `batt_w = 48` (24 small; `icons.h:110`, `2302`). Title label is `LV_ALIGN_TOP_MID` with `title_nudge` (16 / 8) to balance the left mascot. **There is no round-corner handling** — panels are rectangular AMOLED/TFT with 8 px radius panels and a 20 px margin; no board here has a round panel (the 2.06 is a rounded-rect "watch" panel; the 20 px margin plus `col_offset1=23` in the display driver handle that, not `ui.cpp`).

### 2.2 Current usage screen — 480×480 (Large)

`content_w=440`; panels: session y=100–250, weekly y=266–416. Inner offsets are relative to the padded panel (pad x16/y12).

```
x: 0        20                                             460    480
y0 ┌──────────────────────────────────────────────────────────────┐
   │ [mascot 72x48 @x=20,   "Usage"/clock (tiempos_56)   [batt 48x48 @x=412,y=30]
30 │  y≈43..91          centered, +16 nudge, y=30
   │
100│  ┌────────────────────────────────────────────┐ panel_session (20,100) 440×150
   │  │ 42%  (styrene_48)               [Current]  │  pct @ (36,112); pill top-right
   │  │                                            │
168│  │ ████████████░░░░░░░░░░░░░░░░░  bar 408×24  │  bar @ (36,168)
   │  │                                            │
206│  │ Resets in 3h 0m  (styrene_28, dim)         │  reset @ (36,206)
250│  └────────────────────────────────────────────┘
266│  ┌────────────────────────────────────────────┐ panel_weekly (20,266) 440×150
   │  │ 33%                               [Weekly] │
   │  │ ██████░░░░░░░░░                           │  bar @ (36,334)
   │  │ Resets in 5d 12h                           │  reset @ (36,372)
416│  └────────────────────────────────────────────┘
   │
   │            ✻ Pondering…   (mono_32, accent, BOTTOM_MID, y offset −15)   ≈ y 431–465
480└──────────────────────────────────────────────────────────────┘
```

Free vertical budget below the title: y=100…480 (380 px) minus the always-on status line (~34 px + 15 margin) ⇒ **~330 px of content area** on Large.

### 2.3 Current usage screen — 368×448 (Compact)

`content_w=328`; session y=85–215, weekly y=227–357; free strip y=357–~399; status line ≈ y 400–434.

```
x: 0    20                              348   368
y0 ┌──────────────────────────────────────────┐
   │ [mascot]   "Usage" (tiempos_56)   [batt]  │  title_y=30, batt @ (300,30)
85 │ ┌──────────────────────────────────────┐ │ panel_session (20,85) 328×130
   │ │ 42%                       [Current]  │ │
145│ │ ████████████░░░░░░░ 296×24           │ │ bar @ (36,145)
175│ │ Resets in 3h 0m (styrene_28)         │ │
215│ └──────────────────────────────────────┘ │
227│ ┌──────────────────────────────────────┐ │ panel_weekly (20,227) 328×130
   │ │ 33%  …                    [Weekly]   │ │
357│ └──────────────────────────────────────┘ │
   │                                           │
   │        ✻ Pondering…  (mono_32, bottom −15)│ ≈ y 399–433
448└──────────────────────────────────────────┘
```

Note that on Compact the title font is still `tiempos_56` (`L.title_font` default, `ui.cpp:88`; only the bt fonts shrink) and the status line is `mono_32`, so the compact screen is already text-tight horizontally: `styrene_28` ≈ 15 px/char ⇒ ~21 chars in 328−32 = 296 px. Budget OC rows accordingly.

Small (240×240): `content_y=44`, panels 74 px tall, fonts 14–24 ⇒ ~2 panels only; an OC list of >2 rows will not fit — needs a 2-row or single-total fallback.

---

## 3. Data path

### 3.1 Receive path

- Transport: GATT service `4c41555a-…0001`; RX `…0002` is `WRITE | WRITE_NR` (`ble.cpp:342-345`); TX `…0003` `READ|NOTIFY` for ack/nack (`349-352`); REQ `…0004` `NOTIFY` (`354-357`).
- `RxCallbacks::onWrite` (`ble.cpp:263-286`): rejects unencrypted links and non-owner machines, copies into **`rx_buf[512]`** (`BLE_BUF_SIZE 512`, `ble.cpp:15`, `75`), truncating silently at 511 bytes (`ble.cpp:281`), sets `data_ready` and `has_received_data`. **Single-slot buffer, no queue** — a second write before `main.cpp` drains it (`ble_get_data()` clears `data_ready`, `ble.cpp:416-419`) **overwrites** the first. Loop runs every ~5 ms (`main.cpp:398`) so back-to-back writes ≥ ~10 ms apart are safe, but two writes in one burst are not.
- Parse: `parse_json()` (`main.cpp:101-125`), ArduinoJson 7 (`platformio.ini:63`) `JsonDocument` on the heap, `deserializeJson(doc, json)`. Fields: `s sr w wr st c acct tp pd rd t tf ok`. Unknown keys are ignored (the sim relies on this for `name`/`hold_ms`, `ble_sim.cpp:52-53`).
- After parse: `usage_rate_sample`, chime, `ui_update`, `ble_send_ack()` (`main.cpp:374-396`). `ble_send_nack()` on parse failure.
- **Current payload size:** a typical Pro payload is ~100–130 bytes (`daemon/claude_usage_daemon.py:445-467` builds ≤ 13 short keys + clock).

### 3.2 MTU / size constraints

- `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256` in the S3 prebuilt sdkconfig (`~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/sdkconfig:1043`); `ble.cpp` never calls `NimBLEDevice::setMTU`. So a central that runs MTU exchange (CoreBluetooth does, bleak on macOS writes up to `maximumWriteValueLength`, typically 182–512) can push **up to MTU−3 ≈ 253 B per write-without-response**. The daemon uses `response=False` (`claude_usage_daemon.py:625`). A larger payload with WRITE_NR would be truncated/dropped by the central's write limit, not fragmented. Bash/BlueZ uses `WriteValue` with option `0` (`claude-usage-daemon.sh:362`) which is a write-with-response path and can long-write up to 512.
- I did not verify the C6 sdkconfig MTU; assume ≤ 256 too. **Treat ~240 B as the safe ceiling per write** for macOS/Windows; hard cap 511 (`rx_buf`).
- Freshness: `DATA_FRESH_MS = 90000` (`ui.cpp:234`) is set on `last_data_ms` only in `ui_update()` (`ui.cpp:597`). `data_received` / `data_ok` / `view_state` are all module-static in `ui.cpp` and specific to the Claude payload.
- `has_received_data` (`ble.cpp:77`, `285`) is a **BLE-layer** flag: it gates the one-shot refresh request in `ReqCallbacks::onSubscribe` (`ble.cpp:293-298`). Any RX write (even an OC one) sets it. Harmless, but note that an OC-first write would suppress the device's "please refresh" nudge for the *Claude* data. The refresh request is a single `0x01` notify with no payload-type selector (`ble.cpp:441-448`); daemon reacts to it by polling Anthropic (`claude_usage_daemon.py:760`).

### 3.3 Second payload type on RX vs. a new characteristic

**Recommendation: same RX characteristic, discriminated by a new key, routed *before* `parse_json`.**

Why it works:
- Both daemons already write whole JSON blobs to RX; adding a second write per cycle needs no GATT change, no NimBLE `createCharacteristic`, no re-pair, and **no Windows GATT-cache invalidation** (`platformio.ini:31-37` notes Windows caches GATT per bond, so a new characteristic would force a re-pair on that OS).
- `unknown keys ignored` gives graceful degradation: an old firmware that gets an OC payload would parse it as a Claude payload with `ok=false` → idle view (bad, see headline 3) — so **the daemon should only send OC payloads if the user opts in via config** (`opencode = on`, same pattern as `chime`/`clock`, `claude_usage_daemon.py:318-405`), and flag the firmware version requirement.
- Ack/nack semantics stay one channel (TX has no request id, so ack ordering is ambiguous with two payload types, but the daemon does not subscribe to TX anyway, `CLAUDE.md` GATT notes).

Payload sketch (kept under ~240 B; `k` = payload kind; absent/`"cl"` = today's Claude payload, so existing daemons/firmware stay compatible):

```json
{"k":"oc","ok":true,"ts":1785474000,"p":"d",
 "tt":1834000,"c":4.27,
 "m":[["opus-4.5",912000,2.90],["sonnet-4.5",801000,1.10],["gpt-5",121000,0.27]]}
```

Notes on that shape: token counts as integers, cost in dollars as float rounded to 2 dp, model names truncated to ≤ 12 chars daemon-side (the firmware fonts are ASCII-only `0x20-0x7E`, `docs/fonts.md`), at most 4 rows so it fits Small/Compact anyway. ~70 B fixed + ~28 B/row ⇒ ≈ 180 B at 4 rows — inside the safe ceiling.

Routing change in `main.cpp:374-396` (conceptually): peek at the buffer once (`JsonDocument` already used there, or a cheap `strstr(buf,"\"k\":\"oc\"")`) and dispatch to an `oc_parse()` + `ui_update_opencode()` path that **does not** touch `usage_rate_sample`, chime, or `ui_update` (which would set `data_ok`/`last_data_ms`). Send ack as usual.

Why not a new characteristic:
- (+) clean separation, no single-slot overwrite race, could carry larger payloads with its own buffer.
- (−) NimBLE service change, GATT-cache re-pair on Windows, three daemons need discovery of a second char (bash `find_char_path_by_uuid`, `claude-usage-daemon.sh:288`), one more thing for `docs`. Only worth it if the OC payload must exceed ~240 B (e.g. >5 models, per-day history) — in that case chunk on the same char before adding a char.

Race to guard: because `rx_buf` is single-slot, the daemon **must sleep ≥ ~200 ms between the Claude write and the OC write** (or the firmware must copy `rx_buf` to a small queue). Simplest firmware-side fix: make `data_ready` a 2-deep ring in `ble.cpp` — a firmware-only change with no protocol impact.

---

## 4. Fonts & assets pipeline

### 4.1 Fonts

- Pre-compiled LVGL 9 bitmap fonts, `firmware/src/font_*.c`, **4 bpp, uncompressed**, ASCII `0x20–0x7E` only (Mono adds `0xB7, 0x2026, 0x2722, 0x2733, 0x2736, 0x273B, 0x273D`) — recipe `docs/fonts.md:1-40`. Generated with `lv_font_conv` (`npm i -g lv_font_conv`), then **must be hand-patched to LVGL 9** (`docs/fonts.md:42-49`; `CLAUDE.md` gotcha 4): drop `#if LVGL_VERSION_MAJOR` guards, drop `.cache`, add `.release_glyph/.kerning/.static_bitmap/.fallback/.user_data`. Unpatched fonts render invisible. Source OTFs under `assets/` (`StyreneB-Regular.otf`, `TiemposText-400-Regular.otf`, `DejaVuSansMono.ttf`).
- Existing fonts (source-file size; the C source spends ~6 chars per byte of bitmap, so binary ≈ source/6 — my estimate, no build exists; `firmware/.pio` is absent):

| Font | Source | Est. flash | line_height |
|---|---|---|---|
| styrene_12 | 41 KB | ~7 KB | ~13 |
| styrene_14 | 46 KB | ~8 KB | ~15 |
| styrene_16 | 52 KB | ~9 KB | 17 |
| styrene_20 | 65 KB | ~11 KB | 21 |
| styrene_24 | 81 KB | ~14 KB | 25 |
| styrene_28 | 99 KB | ~17 KB | 30 |
| styrene_48 | 235 KB | ~39 KB | 51 |
| tiempos_34 | 141 KB | ~24 KB | ~36 |
| tiempos_56 | 322 KB | ~54 KB | 58 |
| mono_18 | 47 KB | ~8 KB | ~21 |
| mono_32 | 109 KB | ~18 KB | 34 |
| **all 11** | **1.24 MB** | **~210 KB** | |

  Rule of thumb: a new Styrene size costs **~7–40 KB**; a whole additional size family for a new screen is ≪ 100 KB. Existing sizes cover 12/14/16/20/24/28/48 (Styrene) so **no new fonts are required** if the OC screen reuses `pct_font`, `pill_font`, `reset_font`, `pace_font`, `bt_device_font`, `bt_credit_*` fonts. Only a `$` glyph and digits/`.`/`k`/`M` are needed, all in ASCII range. Avoid `·`/`…`/`—` in Styrene (only mono has `0xB7`, `0x2026`).
- Fonts are compiled in for every env (the `.c` files sit at `src/` root and match `+<*>`), so they cost flash on all boards whether or not used; unused const data is not garbage-collected out of `.rodata` unless referenced — only *referenced* fonts are linked (`-Wl,--gc-sections` default with Arduino-ESP32). Adding a font that is referenced costs its full size on every board.

### 4.2 Icons / logos

- `tools/png_to_lvgl.js <in.png> <symbol> [W] [H] [--tint=RRGGBB | --no-tint]` (`tools/png_to_lvgl.js:1-30`) → **RGB565A8 planar**: `w*h` RGB565 then `w*h` alpha bytes (`data_size = w*h*3`, `stride = w*2`; `ui.cpp:349-356`). Default tint white (Lucide PNGs are black-on-transparent). Only the 5 battery icons (×2 sizes) use it; all other art is raw RGB565 baked over its background (`CLAUDE.md` Icons section).
- Sources: `assets/icon_*.png` (Lucide, 24/48 px variants — includes unused `icon_arrow-left/right` PNGs which are natural paging affordances).
- `logo.h` = 80×80 (+ 40×40 small) RGB565 (`logo.h:4-5`, `1210-1211`); `logo_img` is only used on non-PSRAM boards for the static Clawd (`ui.cpp:576-579`). `icons.h` 310 KB source (~50 KB flash).
- A 48×48 RGB565A8 icon = 6.9 KB; 24×24 = 1.7 KB. An OpenCode mark for the header would be ~7 KB (+~2 KB small) — trivial. **Brand caution:** `ATTRIBUTION.md` — the repo bundles proprietary Anthropic assets and has no license; an OpenCode logo needs its own provenance note.

### 4.3 Flash / partition headroom per env (`firmware/platformio.ini`)

| Env(s) | Flash | Partition table | App slot |
|---|---|---|---|
| `waveshare_amoled_216` (S3, original) | board default 8 MB (`esp32-s3-devkitc-1.json:42`) — **no `partitions` line** (`platformio.ini:1-66`) | `default_8MB.csv` (from board JSON `:4`) | **0x330000 = 3.19 MB** |
| `waveshare_lcd_154`, `waveshare_lcd_4`, `waveshare_amoled_18`, `waveshare_amoled_216_c6`, `waveshare_amoled_18_c6` | 16 MB | `default_16MB.csv` (`platformio.ini:80,137,189,262,337`) | 0x640000 = 6.25 MB |
| `waveshare_amoled_206` | 32 MB | `default_32MB.csv` (`:404`) | 0xC80000 = 12.5 MB |

- **No build artifact exists** (`firmware/.pio` absent) and I did not run `pio` (a full ESP32 build with `pioarduino` typically exceeds the 3-minute budget the brief set; the `sim` env would not give app-size numbers anyway). Rough static estimate of what is compiled in: fonts ~0.21 MB + splash animations ~0.4 MB (`CLAUDE.md` says "~400 KB total"; source 845 KB) + `bell_pcm.h` (1.44 MB source, 16-bit samples → est. 0.3–0.4 MB) + icons/logo/clawd ≈ 0.15 MB + LVGL/NimBLE/Arduino/GFX ≈ 1.2–1.6 MB ⇒ **plausibly ~2.5–3 MB**. On the **original 2.16 with its 3.19 MB slot this may be tight — measure with `pio run -e waveshare_amoled_216` (or `-t size`) before adding anything.** Everything else has ≥ 2× headroom. The OC screen's own cost (widgets code + a few string constants + icon) is expected **≲ 20–30 KB**, so it should fit, but confirm on the 8 MB env.
- RAM: LVGL objects are heap allocations (~100–200 B each for labels, ~few hundred for bars/panels); an OC screen with ~30 widgets ≈ 6–10 KB.

---

## 5. Daemon side

### 5.1 macOS — `daemon/claude_usage_daemon.py` (859 lines)

Structure:
- Constants `:27-55` — `DEVICE_NAME`, UUIDs, `POLL_INTERVAL = 60`, `TICK = 5`, `CONNECT_TIMEOUT`, keychain service, `CONFIG_FILE = ~/.config/claude-usage-monitor/config` (`:41`), API URL/headers.
- Config reading: hand-rolled `key = value` parser, re-read per call — `read_chime_setting` (`:318-337`), `read_clock_setting` (`:340-359`); `read_config_dirs` (`:150-172`). Pattern to copy for `opencode = on|off`.
- Payload builders: `poll_api(token)` (`:407-468`) makes the 1-token Haiku call and builds the dict, then `add_chime_field` / `add_clock_fields` (`:466-467`). `poll_active` (`:541-582`) multiplexes several Claude config dirs (`PlanSelector`, `:512-538`). Purely Claude-oriented.
- BLE: `Session.write_payload(payload)` (`:620-629`): `json.dumps(..., separators=(",", ":"))`, `write_gatt_char(RX_CHAR_UUID, data, response=False)`, returns bool. `setup_refresh_subscription` (`:604-620`) for the REQ notify.
- Loop: `connect_and_run` (`:717-800`): connect (bounded 20 s) → subscribe REQ → `while client.is_connected:` poll when `refresh_requested` or `elapsed >= POLL_INTERVAL` (`:760`) → `session.write_payload(payload)` → `wait_for(refresh_requested, TICK)` (`:797-800`).
- **No heartbeat, no `poll_interval` config** on macOS/Windows (grep for `heartbeat` in `daemon/*.py` is empty; only `claude-usage-daemon.sh:112-160`). Fixed 60 s cadence.

**Where the OpenCode collector plugs in:**

1. New module `daemon/opencode_collector.py` with `collect() -> dict | None` that reads OpenCode's local store (SQLite/JSON under its data dir — format is out of scope for this brief; see the other research briefs) and returns the `{"k":"oc",...}` dict. Keep it **sync + fast, run via `asyncio.to_thread`** so SQLite I/O never blocks the BLE loop (the loop is single-threaded and `wait_for(client.connect)` hangs were already a real incident, `:719-735` comments).
2. Config: `read_opencode_setting()` next to `read_clock_setting` (`:340`), default `off`; optional `opencode_interval`.
3. Hook inside `connect_and_run`'s poll branch (`:760`), **after** the Claude write:
   ```python
   if read_opencode_setting() == "on" and time.time() - last_oc >= OC_INTERVAL:
       oc = await asyncio.to_thread(collect_opencode)
       if oc: await asyncio.sleep(0.25); await session.write_payload(oc)
   ```
   Reuse `Session.write_payload` unchanged; it logs the JSON (`:623`) — fine. The 0.25 s gap protects the single-slot `rx_buf` (§3.3).
4. Independence: OC data must still flow when **Claude has no token** (`dead` branch `:773-786`) — the OC block should sit outside the `poll_active` result handling, not nested in `if payload is not None`.
5. Tests: `daemon/tests/` runs on macOS (`CLAUDE.md` Tests); add `test_opencode_collector.py` with a fixture DB, no BLE.

### 5.2 Linux bash — `daemon/claude-usage-daemon.sh` (622 lines)

`poll()` `:484`, `build_payload_for_token` `:369`, `write_gatt` `:349-363` (busctl `WriteValue "aya{sv}"`, builds byte list from the string), main loop `:546-622` with poll/heartbeat/refresh-flag branches (`:597-616`), `read_poll_interval` `:93`, `read_heartbeat_interval` `:114`, `heartbeat()` `:147` replays `LAST_PAYLOAD` with aged `sr/wr/t` (`:132-160`). An OC collector would be a separate shell function calling `python3` (already a dependency, `:5`) or `sqlite3`, and a new branch in the loop. **Heartbeat caveat:** heartbeat only replays the last Claude payload, so OC data would age out unless the OC screen has its own staleness policy (recommend: OC screen shows "updated Xm ago"; no auto-blank).

### 5.3 Windows — `daemon/claude_usage_daemon_windows.py` (777 lines)

Mirror of macOS: `Session.write_payload` `:394-401`, `connect_and_run` `:527-690` with tray state (`tray_windows.py`), `note_write_failure` `:593`. Same plug-in point as macOS; also the reason a new GATT characteristic is unattractive (Windows GATT cache, §3.3).

---

## 6. Simulator

- Env `sim` (`platformio.ini:460-507`, `platform = native`); `-DBOARD_HAS_PSRAM`; excludes `ble.cpp`, `chime.cpp`, `es8311.c` (`:471-473`). SDL2 present on this machine (`/opt/homebrew/bin/sdl2-config`); build+run: `pio run -d firmware -e sim && (cd firmware && .pio/build/sim/program)` (`CLAUDE.md`). Not built by me.
- `boards/sim/ble_sim.cpp` implements `ble.h`. `load_scenario()` (`:80-98`) reads `SIM_SCENARIO` or `sim/scenario.jsonl`; `add_state()` (`:44-63`) stores each JSONL line (≤ `MAX_LINE 512`, ≤ `MAX_STATES 64`) verbatim; `name`/`hold_ms` are extracted only for the title/timing and left in the payload. `ble_tick()` advances every `hold_ms` (`:100-108`); `ble_get_data()` hands the line to `main.cpp` through the **same** `ble_has_data()/parse_json()` path as hardware (`:127-131`). Controls: space play/pause, ←/→ step, 1–9 jump, `d` link toggle, `b/n` buttons, `p` PWR, `c`/`-`/`=` battery (`boards/sim/board.h:6-25`).
- Current scenario: 8 states (`firmware/sim/scenario.jsonl:4-11`), Claude-only.
- **Previewing an OC screen needs no sim changes**: add `{"k":"oc",...,"name":"oc busy day","hold_ms":4000}` lines (routed by the new firmware dispatch), plus a dedicated scenario file (`SIM_SCENARIO=sim/scenario-opencode.jsonl`) so the default Claude scenario is unchanged. Because `ble_get_data()` returns lines in order, interleaving `cl` and `oc` lines exercises the routing (and freshness, since `last_data_ms` must *not* be touched by OC lines).
- Sim caveats: to reach the OC screen the sim needs a way to navigate — mouse click toggles like touch (tap → `global_click_cb`), `p` = PWR, so whatever nav is chosen is testable with mouse + `p`. Screenshot: `s` key or `SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=6000` (`board.h:19-22`); to capture the OC screen headlessly use the documented "temporarily change the boot screen" trick (`main.cpp:234`). Sim window is fixed 480×480 (`board.h:29-30`) — **it cannot show the 368×448 Compact or 240×240 Small layouts** unless `LCD_WIDTH/HEIGHT` are changed locally; those need a sim board-size override (e.g. `-DLCD_WIDTH=368 -DLCD_HEIGHT=448` in a scratch env) or hardware. The sim renders with desktop LVGL and fake data, so it is never a substitute for hardware for panel offsets (`CLAUDE.md`).
- Note the header of `scenario.jsonl` and the simulator note list "parse_json ignores unknown keys" (`ble_sim.cpp:52`) — with the routing change, OC lines never reach `parse_json`.

---

## 7. Risks

1. **C6 has no PSRAM** (`BOARD_HAS_PSRAM` undefined on both `*_c6` envs). LVGL draw strips are 480×20 from internal SRAM (`main.cpp:30-36`); `LV_USE_SNAPSHOT=0`, so **no screenshot** (`main.cpp:132-137`) — the OC screen on C6 must be eyeballed. Keep the screen **pure LVGL labels/bars/panels** (no canvas, no large image buffers, no `lv_snapshot`). Widget heap comes from internal RAM there — avoid dozens of per-row objects; 4 rows × (name label + value label + bar) ≈ 12 objects is fine. No new `heap_caps_malloc(MALLOC_CAP_SPIRAM)` (would return NULL on C6 — the same trap as `CLAUDE.md` gotcha 2).
2. **No `#ifdef BOARD_*` in shared code** (`CLAUDE.md` gotcha 10). Anything board-dependent goes into `BoardCaps`/`compute_layout()`. `#ifdef BOARD_HAS_PSRAM` is the one sanctioned compile-time gate (used at `ui.cpp:552`, `572`, `main.cpp:30,133`); the OC screen should not need it.
3. **Touch conflicts.** Tap on `usage_container` / splash root = toggle (`ui.cpp:480`, `563`); child panels use `LV_OBJ_FLAG_EVENT_BUBBLE` so a tap anywhere hits it. If nav changes, the click handler semantics change for *everyone*. There is **no gesture/swipe code anywhere** (`grep` for `LV_EVENT_GESTURE`/`SWIPE` in `firmware/src` finds nothing in the files read); LVGL `LV_INDEV_TYPE_POINTER` supports `LV_EVENT_GESTURE`, but the touch HALs latch a single point and `touch_hal_read` must finish in << 5 ms (`docs/porting/hal-contract.md:35`) — swipe would work but is untested on 7 boards; the first tap after sleep is swallowed (`main.cpp:67-89`). Prefer tap-based paging.
4. **PWR overloading.** PWR short already means "cycle animation" (splash) / "cycle brightness" (elsewhere). If PWR pages screens, brightness cycling needs a new home (e.g. long-press is taken by pairing at 1.5 s+; the pair gesture also uses release edges) — don't touch pairing (`main.cpp:242-286`).
5. **`data_ok`/`last_data_ms` are Claude-specific statics in `ui.cpp`** — OC data must have its own freshness (its cadence will differ from 60 s) and not pull the usage screen into/out of idle.
6. **Idle/sleep**: 30 min timeout, `IDLE_SLEEP_WHEN_CHARGING=false` (`idle_cfg.h`); the current screen is remembered in `current_screen` across sleep; no per-screen wake behaviour needed.
7. **Rotation:** only the 2.16 S3 rotates via CPU strip remapping (`has_rotation`); the OC layout on 2.16 must tolerate 90° rotation transitions the same as the usage screen (both are plain LVGL, so it is inherited for free, provided the screen is 480×480-square-safe — it is).
8. **Screen real estate on Small (240×240)** fits two 74-px panels at best; OC needs a degraded 2-row layout there, or hide the per-model list and show total tokens + cost only.
9. **Unknown/malformed OC payload ⇒ nack.** Firmware must ack/nack based on OC parse, not Claude parse, so the daemon can tell.
10. **Font glyphs:** Styrene/Tiempos are ASCII-only. Real model names may include non-ASCII or long strings (`claude-opus-4-5-20251101`); sanitize/truncate **daemon-side** (label overflow otherwise; use `LV_LABEL_LONG_DOT` on the firmware side as belt-and-braces).
11. **Docs drift:** `CLAUDE.md` still lists a Bluetooth screen, `SCREEN_CONTROLLER`, 3-screen UI and "PWR → cycle screens"; update it alongside the change so future sessions aren't misled (and `docs/porting/*` for any new `BoardCaps` field).
12. **Licensing/provenance** of any OpenCode logo/branding (`ATTRIBUTION.md`).
13. **Legacy-firmware compatibility:** a new daemon sending OC payloads to an *un-updated* device will bounce the Claude screen to idle (headline 3). Gate on config (`opencode = on`) and document; optionally the daemon can skip OC writes if the device never acks OC (ack is `{"ack":true}` for both, so use a firmware-version field in the device name or a new capability bit if this matters).

---

## Recommended integration plan

**Files to touch**

- `firmware/src/ui.h` — `SCREEN_OPENCODE`; declare `ui_update_opencode(const OcData*)`, `ui_next_screen()`.
- `firmware/src/ui.cpp` — Layout fields + 3 breakpoints; `init_opencode_screen()`; widgets (header/title, total-tokens+cost hero panel, up-to-4 model rows with bar+tokens+cost, "updated Xm ago" label); own freshness state (`oc_last_data_ms`, `OC_FRESH_MS` ≈ 5–10 min, distinct from Claude's 90 s); extend `ui_show_screen`, `global_click_cb`, `ui_toggle_splash`, `ui_tick_anim` (don't early-return for the OC tick), `apply_battery_visibility`.
- `firmware/src/data.h` — new `struct OcData { bool valid; long ts; long total_tokens; float cost; uint8_t n; struct { char name[14]; long tokens; float cost; } m[4]; }` (~130 B static).
- `firmware/src/main.cpp` — dispatch at `:374-396`: `k=="oc"` → `oc_parse()` → `ui_update_opencode()` → ack/nack, **skipping** `usage_rate_sample`/chime/`ui_update`; update PWR handler `:343-350`; add `oc_parse` next to `parse_json` (`:101`).
- `firmware/src/ble.cpp` (optional but recommended) — 2-deep RX ring instead of single `rx_buf` (`:75`, `263-286`, `412-419`) to remove the overwrite race.
- `firmware/sim/scenario-opencode.jsonl` (new) — interleaved `cl`/`oc` states; leave `scenario.jsonl` alone.
- `daemon/opencode_collector.py` (new) + `daemon/claude_usage_daemon.py` (`read_opencode_setting`, hook in `connect_and_run` `:760`) + `daemon/tests/test_opencode_collector.py`.
- Follow-ups (same feature, lower priority): `claude_usage_daemon_windows.py` `:646`, `claude-usage-daemon.sh` loop `:597-616`.
- Docs: `CLAUDE.md` (screens list, PWR behaviour, payload spec), `README.md`.

**Payload strategy**

- Same RX characteristic, discriminator `"k":"oc"` (**not** `"t"` — already the clock epoch, `main.cpp:120`); Claude payloads stay unchanged (absent `k` ⇒ Claude).
- ≤ ~240 B per write (MTU 256 − 3, `sdkconfig:1043`), 4 model rows max, names ≤ 12 ASCII chars, cost as 2-dp float, tokens as ints. Daemon cadence for OC independent of the Claude poll (e.g. every 60–120 s + on state change), ≥ 250 ms after the Claude write, gated by `opencode = on` in `~/.config/claude-usage-monitor/config`.
- OC freshness handled in firmware ("updated Xm ago"), no interaction with `DATA_FRESH_MS`/idle/`usage_rate`.
- New characteristic only if a future payload must exceed ~240 B; prefer chunking first.

**Navigation proposal (touch-first; no per-board `#ifdef`)**

| Board class | Proposal |
|---|---|
| All boards (incl. LCD-4, 1 button, no PWR) | Tap cycles **Splash → Usage → OpenCode → Splash** (replace the 2-state toggle with a small array `order[]`). Preserves "tap once from splash lands on the last non-splash screen" via `prev_non_splash_screen`. Alternative with less behaviour change: tap toggles splash↔last-non-splash as today, and **a tap on the title/header band pages Usage↔OpenCode** (`lv_obj` hit-area ~60 px tall, avoids 480 px-wide accidental hits). Recommend the cycle for simplicity; the header-band variant if users complain about extra taps to reach splash. |
| 1-button + PWR (1.8 S3/C6, 2.06), 2-button + PWR (2.16 S3/C6, LCD-1.54) | Same taps. PWR short keeps today's meaning (splash → next animation; else brightness) — **do not repurpose**; it also has no equivalent on LCD-4, so behaviour stays uniform. Optionally add PWR *double-tap* paging later. |
| BOOT / SECONDARY | unchanged (HID Space / Shift+Tab). |

Pairing gesture, wake-swallow and idle logic untouched.

**Layout / font / asset budget**

- Reuse existing fonts only (no new `.c`): `pct_font` (hero tokens/cost), `pill_font`/`reset_font` (row labels), `pace_font`/`bt_credit_*` (secondary text). Flash impact from fonts: **0 KB**.
- Layout: Large (≥460) hero panel 150 px + up to 4 rows ≈ 40 px each ⇒ ≈ 330 px content budget (§2.2); Compact hero 110 + 4×36; Small: hero only + 2 rows. New fields in `compute_layout()` (`oc_row_h`, `oc_rows_max`, fonts) — the sanctioned way to add sizes, no per-board code.
- Optional icons: OpenCode mark 48×48 + 24×24 RGB565A8 ≈ 9 KB total via `tools/png_to_lvgl.js` (provenance per `ATTRIBUTION.md`).
- Estimated total flash: **≲ 20–30 KB code+data** — negligible on 16/32 MB envs; **verify against the 3.19 MB app slot of `waveshare_amoled_216`** (`default_8MB.csv`; no build artifact available to measure now). RAM: ~30 LVGL objects ≈ 6–10 KB heap, safe on C6.
- Verification path: sim first (`SIM_SCENARIO=firmware/sim/scenario-opencode.jsonl`, mouse taps to navigate, `s` for BMP), then a scratch 368×448 sim size for Compact, then hardware for C6 (no screenshot), and a Small/240 eyeball on LCD-1.54.
