# Single-board reorganisation: Waveshare ESP32-S3-Touch-AMOLED-2.16 only

**Status:** in progress in the working tree (board folders staged for deletion,
`firmware/platformio.ini` already trimmed, `firmware/partitions/` added,
README header already edited).

**Scope:** engineering plan for collapsing the firmware to one supported board
plus the desktop simulator, and for laying down OTA-ready dual-slot
partitioning. This document describes the change; it does not implement BLE OTA
firmware (that is [`design/ota-ble/DESIGN.md`](../ota-ble/DESIGN.md)).

---

## 1. Goal

The fork now targets **one** physical board:

> **Waveshare ESP32-S3-Touch-AMOLED-2.16** — ESP32-S3, 480×480 CO5300 AMOLED,
> 16 MB flash, NimBLE peripheral driven by the macOS/Linux/Windows Python
> daemon over a custom GATT service.

Collapsing to a single board:

- removes seven ports and their C6/SoC variants that are no longer maintained
  or testable here;
- makes the partition table a first-class artefact (dual OTA slots) instead of
  a stock `default_16MB.csv`;
- keeps exactly one non-hardware target — the native SDL2 simulator at
  480×480 — so UI work can continue without a panel.

The firmware architecture is unchanged: shared code in `firmware/src/` still
talks to a HAL, and `boards/waveshare_amoled_216/` is the only HAL
implementation compiled into a hardware build.

## 2. Decisions

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Only `env:waveshare_amoled_216` is supported | Single maintained board; CI/build surface halved; no dead HAL code to keep compiling. |
| D2 | `env:sim` is kept at **480×480** | The simulator is the only way to iterate on UI/animations and capture screenshots without hardware. 480×480 matches the real panel 1:1. |
| D3 | `env:sim_368` and `env:sim_240` are removed | They existed to check the 368×448 (1.8) and 240×240 (1.54) layout breakpoints. With those boards gone there is no breakpoint to check. |
| D4 | All other board folders and `[env:...]` blocks are removed | No supported hardware, no code owner, no QA. |
| D5 | `docs/porting/` is removed | The HAL contract is now internal to one port; the porting walk-through documents a workflow the fork no longer offers. |
| D6 | `screenshots/amoled_18/` is removed | Screenshots of a removed board. |
| D7 | Partitioning moves to OTA-ready dual-slot via a repo-owned CSV (`firmware/partitions/waveshare_amoled_216.csv`) | Enables BLE OTA and rollback later without a repartition; keeps the layout under version control. |
| D8 | No WiFi stack is added now | OTA is designed to ride the existing BLE link (see DESIGN doc). |

## 3. Inventory

### 3.1 Removed

**Board folders** (all files under each — HAL, pins, drivers, per-rev detection):

```
firmware/src/boards/template/
firmware/src/boards/waveshare_amoled_18/       # incl. board_rev.h, io_expander.*
firmware/src/boards/waveshare_amoled_18_c6/    # incl. io_expander.*
firmware/src/boards/waveshare_amoled_216_c6/
firmware/src/boards/waveshare_amoled_206/
firmware/src/boards/waveshare_lcd_154/
firmware/src/boards/waveshare_lcd_4/           # incl. io_expander.*
```

**PlatformIO envs** (in `firmware/platformio.ini`):

```
waveshare_lcd_154
waveshare_lcd_4
waveshare_amoled_18
waveshare_amoled_216_c6
waveshare_amoled_18_c6
waveshare_amoled_206
sim_368
sim_240
```

**Docs and assets:**

```
docs/porting/adding-a-board.md
docs/porting/hal-contract.md
docs/porting/capability-flags.md
screenshots/amoled_18/splash.png
screenshots/amoled_18/usage.png
```

**README / usage text:** the multi-board "Boards supported out of the box"
list, the alternative-port PR note, and the "Porting to another board"
paragraph (already removed from the README in the working tree); the
`Development → Porting` link; the `sim_368` / `sim_240` sections of
`SIM-USAGE.md`; and the stale `waveshare_amoled_18` example in the
`flash.sh` / `flash-mac.sh` usage comments (comment-only — the scripts scrape
env names from `platformio.ini`, so no logic changes).

### 3.2 Kept

| Path | Why |
|------|-----|
| `firmware/src/boards/waveshare_amoled_216/` | The only hardware HAL implementation. |
| `firmware/src/boards/sim/` | Native SDL2 simulator (480×480). |
| `firmware/src/hal/` and all shared `firmware/src/*.cpp/.h` | Board-agnostic core (UI, splash, data parsing, BLE, idle, etc.). |
| `firmware/sim/` scenarios | Simulator input. |
| `firmware/partitions/waveshare_amoled_216.csv` | New OTA-ready layout (D7). |
| `docs/fonts.md` | Still relevant (LVGL fonts / patching). |
| `daemon/`, `install*.sh`, `install-windows.ps1`, `flash*.sh` | Daemon and installers are board-agnostic. |
| `research/`, `tools/`, `assets/`, `cooked-ideas/` | Sprite/icon provenance and tooling, independent of board count. |
| `design/opencode-screen/` | Historical design archive; referenced names are historical, not live config. |

> `CLAUDE.md` is reduced to the single-board prose alongside this reorg
> (tracked separately; this documentation task does not edit it). The
> `design/opencode-screen/` briefs and research notes still mention the removed
> boards — they are a historical archive, not build inputs. Treat residual
> references there as historical.

## 4. Exact changes

### 4.1 `firmware/platformio.ini`

The file is reduced to two env blocks.

- Delete every `[env:...]` block listed in §3.1 and their leading comments.
- Keep `[env:waveshare_amoled_216]`; add the flash-size and partition keys:

```ini
[env:waveshare_amoled_216]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.38-1/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
board_build.arduino.memory_type = qio_opi
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.partitions = partitions/waveshare_amoled_216.csv
upload_speed = 921600
monitor_speed = 115200

build_src_filter =
    +<*>
    -<boards/>
    +<boards/waveshare_amoled_216/>
```

`build_flags` and `lib_deps` for this env are unchanged (NimBLE roles, PPCP
timing, LVGL flags, GFX/SensorLib/XPowersLib/ArduinoJson/NimBLE deps).

- Keep `[env:sim]` as-is (480×480), still excluding `ble.cpp`, `chime.cpp`
  and `es8311.c`.
- The `; ---- Sim variants ---` comment block and both `[env:sim_368]` /
  `[env:sim_240]` blocks are deleted.

### 4.2 New partition CSV — `firmware/partitions/waveshare_amoled_216.csv`

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x600000,
app1,     app,  ota_1,   0x610000, 0x600000,
spiffs,   data, spiffs,  0xC10000, 0x3E0000,
coredump, data, coredump,0xFF0000, 0x10000,
```

Sizing check (16 MB = `0x1000000`): each app slot is 6 MiB; the table runs
from `0x9000` to `0x1000000`. `otadata` is present, which is what makes the
dual-slot rollback in the OTA design possible.

### 4.3 Host docs and scripts

| File | Change |
|------|--------|
| `README.md` | Hardware section already reduced to the single board + "dual OTA-ready app slots (no OTA feature is implemented yet)"; remove the `Development → Porting` bullet and any `docs/porting` links. |
| `SIM-USAGE.md` | Remove the `sim_368` / `sim_240` build-and-run block; keep the 480×480 `env:sim` instructions. |
| `CLAUDE.md` | Reduced to the single-board context, the two build targets, and the OTA-ready partition note (tracked with the reorg; not edited by this documentation task). |
| `docs/porting/*` | Delete the folder. |
| `screenshots/amoled_18/*` | Delete the folder. |
| `flash.sh`, `flash-mac.sh` | Update the example board in the usage comments only (`waveshare_amoled_216`). No logic change: both scrape `[env:...]` from `platformio.ini`. |
| `firmware/src/boards/sim/board.h` | Optional comment cleanup: drop the `sim_368` / `sim_240` sentence; geometry defaults stay 480×480. |

## 5. Verification checklist

1. **Hardware build**
   ```bash
   pio run -d firmware -e waveshare_amoled_216
   ```
   Must compile with no board-2xx/1xx symbols.

2. **Simulator build**
   ```bash
   pio run -d firmware -e sim
   cd firmware && .pio/build/sim/program     # 480×480 window opens
   ```

3. **Flash to hardware** (optional, with a device connected):
   ```bash
   ./flash-mac.sh waveshare_amoled_216       # or ./flash.sh ... on Linux
   ```
   First flash to the new layout: if NVS/otadata offsets shifted, a full
   erase (`pio run -t erase`) before upload is the safe path; afterwards
   normal OTA uploads work.

4. **Partition sanity**
   ```bash
   pio run -d firmware -e waveshare_amoled_216 -t size
   ```
   Confirm the app image fits comfortably in a 6 MiB slot and the target
   partition is `partitions/waveshare_amoled_216.csv`.

5. **No dangling references** — these should return only historical archives
   (`design/opencode-screen/`, `CLAUDE.md`) or nothing:
   ```bash
   grep -rIn --exclude-dir=.git --exclude-dir=.pio \
     -e waveshare_amoled_18 -e waveshare_amoled_206 \
     -e waveshare_lcd_154 -e waveshare_lcd_4 \
     -e waveshare_amoled_216_c6 -e boards/template \
     -e docs/porting -e sim_368 -e sim_240 .
   ```
   In particular, no live config (`firmware/platformio.ini`,
   `firmware/partitions/`, `flash*.sh`, `README.md`, `SIM-USAGE.md`) may
   reference a removed board or env.

6. **Include graph** — no shared source `#include`s a removed board header:
   ```bash
   grep -rIn --exclude-dir=.git --exclude-dir=.pio '#include "boards/' firmware/src | grep -v waveshare_amoled_216 | grep -v boards/sim
   ```
   (Should be empty; shared code should only touch `hal/`.)

## 6. Rollback

Every change is tracked in git and nothing is irreversible:

```bash
git checkout -- firmware/platformio.ini README.md   # restore files
git restore --staged --worktree firmware/src/boards  # undo staged deletions
rm -rf firmware/partitions                            # remove untracked CSV
```

The only hardware-side effect is the new partition table: reflashing an old
single-slot layout over it needs a full erase (or the reverse on the way
back). No data is destroyed by the repository change itself.

## 7. Out of scope

- **BLE OTA firmware implementation** — the state machine, the GATT
  characteristics, `esp_ota_*` wiring, boot-confirm/rollback, signing, and the
  Python uploader are specified in [`design/ota-ble/DESIGN.md`](../ota-ble/DESIGN.md).
- WiFi provisioning / any network stack.
- Reworking the daemon's telemetry path or the HID service.
- Rewriting `CLAUDE.md` or the `design/opencode-screen/` archive.
