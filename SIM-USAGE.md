# Desktop simulator (`-e sim`) — usage

The native desktop simulator runs the full firmware loop in an SDL2 window
standing in for the AMOLED panel — `main.cpp`, `ui.cpp`, `splash.cpp`, idle
fade, pair gesture, JSON parsing, and usage-rate/chime logic all run
unmodified. Only `ble.cpp`/`chime.cpp` are swapped for stubs. Sources live in
`firmware/src/boards/sim/` (HAL against SDL2 + Arduino shims in `shim/`), with
scenario data in `firmware/sim/`.

## Build & run

```bash
sudo apt install libsdl2-dev        # one-time (macOS: brew install sdl2)
pio run -d firmware -e sim
cd firmware && .pio/build/sim/program
```

Launch from the `firmware/` directory — the default scenario path
(`sim/scenario.jsonl`) is resolved relative to it.

## Controls

| Key | Action |
|---|---|
| mouse / left-drag | touch (tap = next screen) |
| `space` | play/pause scenario playback |
| `←` / `→` | step one scenario state (pauses playback) |
| `1`–`9` | jump to scenario state N (pauses playback) |
| `d` | toggle BLE connected/disconnected |
| `b` (tap) | PRIMARY (BOOT) — previous screen |
| `b` (hold 300 ms) | PRIMARY — HID Space held down (voice-mode PTT) |
| `n` (tap) | SECONDARY — next screen |
| `n` (hold 300 ms) | SECONDARY — HID Shift+Tab held down (mode toggle) |
| `p` | PWR button (short: next animation/scene or brightness; hold ~3s + release = pair gesture) |
| `c` | toggle charging |
| `-` / `=` | battery down / up 5% |
| `s` | save screenshot BMP to the current directory |
| `esc` / window close | quit |

Full, authoritative map: `firmware/src/boards/sim/board.h`.

## Scenarios

`firmware/sim/scenario.jsonl` plays in a loop — one JSON object per line, the
daemon payload plus two optional keys:

- `"name"` — shown in the window title
- `"hold_ms"` — time on this state (default 3000)

Lines starting with `#` are comments. Lines containing an `"ss"` array are
**session payloads** (issue #135 wire format) and go out on the session
characteristic path; everything else is a quota payload.

Session row format:

```
[sid, label, state, ctx%, elapsed_s, model, tool, ntools, nagents, tdone, ttotal, tok]
```

States: 0 starting · 1 idle · 2 thinking · 3 responding · 4 running-tool ·
5 compacting · 6 needs-permission · 7 asking-you · 8 needs-input · 9 error.
`tok` is context tokens in 1k units (190 = 190k); `-1`/absent = unknown.

### OpenCode scenario

`firmware/sim/scenario-opencode.jsonl` interleaves Claude beats with OpenCode
ones tagged `"k":"oc"`, exactly as the daemon sends them (the firmware routes on
that tag). It covers every state the OpenCode screens have: idle, one session,
two sessions, near the limit, limit reached, a window that just reset (which
triggers the `assemble` splash scene), the estimated fallback, and
consumption-only (no OpenCode Go key).

```bash
SIM_SCENARIO=sim/scenario-opencode.jsonl .pio/build/sim/program
```

## Environment variables

| Variable | Effect |
|---|---|
| `SIM_SCENARIO` | scenario file to play (built-in state list if missing) |
| `SIM_START_SCREEN` | `splash` \| `usage` \| `oc_splash` \| `oc_usage` — jump straight to a screen |
| `SIM_BUTTONS` | `1` emulates a board with no SECONDARY button |
| `SIM_AUTOSHOT_MS` | headless: screenshot after N ms, then exit |
| `SIM_AUTOSHOT_PATH` | headless: screenshot filename (default `sim-autoshot.bmp`) |

`SIM_START_SCREEN` exists because booting always lands on the Clawd splash and
the page dots only show for 1.5 s, so an autoshot timed off the boot would never
capture the screen under test. The jump is applied once, as soon as the scenario
has delivered both payload kinds (an OpenCode screen with no payload is blank) or
after 1.5 s, so it works with a Claude-only scenario too. `SIM_BUTTONS=1` is
read at run time rather than baked in, so one binary can check both a
two-button and a one-button board.

## Headless screenshots (CI-friendly)

```bash
SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=6000 .pio/build/sim/program
```

Saves `sim-autoshot.bmp` (override with `SIM_AUTOSHOT_PATH`) after the given
delay and exits. Combine with `SIM_SCENARIO` pointing at a single-state file,
plus `SIM_START_SCREEN`, to capture any specific screen:

```bash
SIM_SCENARIO=sim/scenario-opencode.jsonl SIM_START_SCREEN=oc_usage \
  SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=3000 .pio/build/sim/program
```

## Caveat

The sim mirrors the S3 2.16 geometry (480×480) but renders with desktop LVGL and
fake data. It's ideal
for iterating UI layouts, but panel-level behavior — column offsets, rotation,
flush rounding — lives in the hardware board folders, so always do a final check
on real hardware before merging panel-related changes.
