# Clawdmeter

> **Personal fork.** This is a fork of
> [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter).
> All credit for the original work goes to that author. See
> [ATTRIBUTION.md](ATTRIBUTION.md) — including the licensing status, which
> matters if you plan to redistribute this. The README below is adapted from
> the upstream one: this fork supports a single board only.

> Also check out [Beam](https://github.com/notaharness/beam)! A CLI that lets you pair your machines using a passkey and [@Tailscale's tailcat](https://tailscale.com/blog/tailcat).

<img src="assets/readme/waving.gif" width="120" align="right" alt="">

A small ESP32 dashboard I made for my desk to keep an eye on Claude Code usage.

It runs on a [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) and pairs over Bluetooth, the splash screen plays pixel-art Clawd animations that get
busier when your usage rate climbs. The two side buttons send Space and
Shift+Tab over BLE HID for Claude Code's voice mode and mode-toggle shortcuts.

<img width="1179" height="994" alt="Usage meter" src="https://github.com/user-attachments/assets/83e54aea-0932-428f-94aa-b3ede3a360aa" />

## Screens

The device boots into the splash. Tap the screen anywhere to switch to the Usage view; tap again to flip back to the splash. With [OpenCode screens](#opencode-screens) enabled the cycle grows to four screens.

|              Splash               |              Usage              |
| :-------------------------------: | :-----------------------------: |
| ![Splash](screenshots/splash.gif) | ![Usage](screenshots/usage.png) |
|   Splash; touch-toggle anytime    | Session and weekly utilization  |

While the splash is up, the middle (PWR) button cycles animations. **Hold the power button for 3 seconds, then release, to put the device into pairing mode** — this clears the saved Bluetooth bond and re-advertises. The firmware also auto-rotates animations every 20 s within the current usage-rate group, so a long stretch on the splash isn't just one Clawd on loop.

## OpenCode screens

Two more screens report your [OpenCode](https://opencode.ai) usage next to
Claude's. The first is an animated splash built from OpenCode's own material —
the mark, the wordmark, and the terminal block scanner — that types itself out
while you're idle, speeds up while sessions are running, turns amber as a limit
approaches and freezes when you reach one. The second is a usage screen with the
5-hour, weekly and monthly bars, 7-day tokens, your top model and a live session
line. The type is IBM Plex Mono, the alternative opencode.ai itself names.

Navigation is the same four-screen cycle — Claude splash → Claude usage →
OpenCode splash → OpenCode usage — with the side buttons: **tap** to move
between screens, **hold** to send Space / Shift+Tab as before (see
[Physical buttons](#physical-buttons)). A row of dots at the bottom shows where
you are for a second and a half after each change. Until the first OpenCode
payload arrives, the cycle stays the two Claude screens and nothing else changes.

### Enabling them

Only the **macOS daemon** collects OpenCode data so far, and it is off by
default. Turn it on in the daemon config file:

```bash
echo "opencode = on" >> ~/.config/claude-usage-monitor/config
```

After changing the config, **restart the running daemon** to load the OpenCode collector module:

```bash
launchctl kickstart -k gui/$(id -u)/com.user.claude-usage-daemon
```

If the LaunchAgent was installed from another checkout, re-run `./install-mac.sh` from this one so the plist points at this repo.

The daemon then polls every 60 s, on its own beat so the OpenCode screens keep
working even when Claude has no token. It reads
`~/.local/share/opencode/opencode.db` **read-only** for tokens, spend, top model
and session activity, and if you have an OpenCode Go key in `auth.json` it also
queries OpenCode's official usage endpoint for exact percentages and reset
countdowns. What reaches the screen depends on that key:

- **With a Go key** — the exact percentages and reset countdowns from the
  endpoint. If the endpoint is unreachable or errors, the daemon falls back to a
  local estimate and the chips are marked `· est.` to say so.
- **Without one** — consumption-only mode: today's and 7-day tokens and spend in
  place of percentages, with no bars. No network call is made at all.

Neither file is written to, and the key is never logged.

## Hardware

The only supported hardware is the [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) — a 480×480 AMOLED touch panel with 16 MB flash and dual OTA app slots (see [Firmware updates](#firmware-updates-hybrid-ota)).

## Prerequisites

- Linux (tested on Ubuntu), macOS, or Windows 10/11
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Linux: `curl`, `bluetoothctl`, `busctl`, `dbus-monitor` (BlueZ Bluetooth stack), `python3`, `setsid`, `stdbuf` (util-linux / coreutils), `systemctl` (systemd user services)
- macOS: **Python 3.10+** (the installer sets up a venv with `bleak` and `httpx`; if not found, run `brew install python`)
- Windows: `python3` 3.11+ (the installer sets up a venv with `bleak`, `httpx`, and `pystray`)
- Claude Code with an active subscription

## macOS installation

The macOS host pieces — Python daemon, LaunchAgent, and flash helper — were ported by [Chris Davidson (@lorddavidson)](https://github.com/lorddavidson). Thanks Chris!

### Flash the firmware

```bash
./flash-mac.sh                                             # waveshare_amoled_216, auto-detects /dev/cu.usbmodem*
./flash-mac.sh waveshare_amoled_216 /dev/cu.usbmodem1101   # or pass an explicit USB serial port
```

`flash-mac.sh` defaults to the `waveshare_amoled_216` env and auto-detects the USB serial port (`/dev/cu.usbmodem*`).

### Pair the device

After flashing, open **System Settings → Bluetooth** and click _Connect_ next to "Clawdmeter". The daemon only ever connects to the peripheral this Mac is paired/connected to — it never scans for a nearby device — so once it's connected here the daemon picks it up on its next poll (~60 s).

### Install the daemon

The daemon reads your Claude OAuth token from the macOS Keychain (service `Claude Code-credentials`), polls usage every 60 s, and pushes it to the display over BLE.

```bash
./install-mac.sh
```

The installer creates a Python venv in `daemon/.venv/`, installs `bleak` and `httpx`, renders a LaunchAgent into `~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`, and loads it. The first run is launched interactively so macOS prompts for Bluetooth permission.

Useful commands:

```bash
launchctl list | grep claude-usage                                          # check it's running
tail -F ~/Library/Logs/claude-usage-daemon.out.log                          # live logs
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist  # stop
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist # start
```

## Linux installation

### Flash the firmware

```bash
./flash.sh                                    # waveshare_amoled_216 on /dev/ttyACM0
./flash.sh waveshare_amoled_216 /dev/ttyACM1  # or pass an explicit USB serial port
```

`flash.sh` defaults to the `waveshare_amoled_216` env and `/dev/ttyACM0`.

### Pair the device

After flashing, the device advertises as "Clawdmeter". Pair it once:

```bash
# Scan for the device
bluetoothctl scan le

# When "Clawdmeter" appears, pair and trust it
bluetoothctl pair F4:12:FA:C0:8F:E5    # use your device's MAC
bluetoothctl trust F4:12:FA:C0:8F:E5
```

To re-pair later, hold the power button for 3 seconds then release — the device clears its saved bond and re-advertises.

### Install the daemon

The daemon polls your Claude usage every 60 seconds and sends it to the display over BLE.

```bash
./install.sh
systemctl --user start claude-usage-daemon
```

Check status: `systemctl --user status claude-usage-daemon`

View logs: `journalctl --user -u claude-usage-daemon -f`

To change the poll interval, set `poll_interval = <seconds>` in `~/.config/claude-usage-monitor/config` (see `daemon/config.example`). The daemon picks it up without a restart. Polling slower than ~80s is fine: between polls the daemon replays the last payload every `heartbeat_interval` seconds (default 60) with the reset countdowns aged, so the firmware's 90s freshness window never lapses and no reflash is needed.

## Windows installation

Runs natively on Windows — no WSL required. A system-tray app polls your usage and pushes it over BLE, and starts automatically at login.

### Prerequisites

- **Native Windows** (not WSL).
- **Python 3.11+** from [python.org](https://www.python.org/downloads/) — check _"Add python.exe to PATH"_ during install.
- **Claude Code** installed, with `claude login` completed. The token is read from `%USERPROFILE%\.claude\.credentials.json` (falling back to `%LOCALAPPDATA%\Claude\` then `%APPDATA%\Claude\`).
- The repo on a **native Windows path** (e.g. `%USERPROFILE%\Clawdmeter`), **not** a `\\wsl$` share — the installer refuses a WSL path.

### Flash the firmware

```powershell
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port COM5   # use your device's COM port (COMx)
```

The env is `waveshare_amoled_216`; the port is your device's COM port on Windows.

### Pair the device

The device is a bonded BLE HID keyboard, so pair it once: **Settings → Bluetooth & devices → Add device → Bluetooth**, then select "Clawdmeter". Pairing is **required** — it enables the physical buttons and keeps a persistent connection (the device keeps showing your last-synced usage even after the daemon quits). To undo, use **Remove device** (this disables the buttons).

### Install the daemon (recommended)

From the repo root in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

This creates a venv, installs `bleak`/`httpx`/`pystray`/`Pillow` from the in-repo requirements (no internet downloads), registers a per-user login-autostart entry (`HKCU\…\Run`, no admin needed), and launches the tray app headlessly (no console window).

### Run manually instead (optional)

```powershell
python -m venv .venv
.venv\Scripts\Activate.ps1        # if blocked: Set-ExecutionPolicy -Scope CurrentUser RemoteSigned, then retry
pip install -r daemon\requirements-windows.txt
python daemon\claude_usage_daemon_windows.py        # runs in the foreground; Ctrl+C to stop
```

### Tray icon and menu

The icon's corner bubble shows state — **green** Connected, **amber** Scanning, **red** Error — and hovering shows the status (`Connected · last update HH:MM`). A notification fires once when it enters Error (e.g. an expired token). Right-click for the menu:

- **Status header** — live state + last sync time.
- **Start at login** — toggle autostart on/off.
- **Quit** — stops the daemon cleanly; leaves the Windows pairing intact (device keeps its last reading).

### Logs and troubleshooting

```powershell
Get-Content $env:LOCALAPPDATA\Clawdmeter\daemon.log -Tail 30        # view logs
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Clawdmeter /f   # remove autostart
```

| Symptom                                | Fix                                                      |
| -------------------------------------- | -------------------------------------------------------- |
| `Device not found`                     | Power on the device; make sure it's in range and paired. |
| `token expired` toast / `API HTTP 401` | Re-run `claude login`, then restart the daemon.          |
| `Connection failed`                    | Toggle Windows Bluetooth off/on in Settings.             |
| `Warning: running under Linux/WSL`     | Run from a native PowerShell window, not a WSL shell.    |

## How it works

<img src="assets/readme/magnifier.gif" width="150" align="right" alt="">

1. The daemon reads your Claude Code OAuth token — from the macOS Keychain (service `Claude Code-credentials`) on macOS, or from `~/.claude/.credentials.json` on Linux (`%USERPROFILE%\.claude\.credentials.json` on Windows).
2. It makes a minimal API call to `api.anthropic.com/v1/messages` — one token of Haiku, basically free.
3. The usage numbers come straight out of the response headers (`anthropic-ratelimit-unified-5h-utilization` and friends).
4. The daemon connects to the ESP32 over BLE and writes a JSON payload to the GATT RX characteristic.
5. The firmware parses it and updates the LVGL dashboard.
6. The firmware also tracks the rate of change of session % over a 5-minute window and picks splash animations from the matching mood group.
7. The two side buttons are independent of all of this — they send Space and Shift+Tab as BLE HID keyboard input to the paired host directly.

## Physical buttons

The board has three side buttons. The two side buttons are **tap or hold**: a short tap walks the screen cycle, a hold sends the HID key. The middle (PWR) button steps the animation or scene on a splash, cycles brightness on the usage screens, and, held for 3 seconds, triggers pairing mode.

| Button           | GPIO         | Tap                                                       | Hold                                                 |
| ---------------- | ------------ | --------------------------------------------------------- | ---------------------------------------------------- |
| **Left**         | GPIO 0       | Previous screen                                          | Space (Claude Code voice-mode push-to-talk)          |
| **Middle** (PWR) | AXP2101 PKEY | On a splash: next animation or scene. On usage: brightness | 3 s + release: pairing mode                          |
| **Right**        | GPIO 18      | Next screen                                              | Shift+Tab (Claude Code mode toggle)                  |

Space and Shift+Tab go out as standard BLE HID keyboard reports, so they trigger in whatever window has focus on the paired host — not just Claude Code. Tapping the panel always moves to the next screen. Telling a tap from a hold means the HID keys now reach the host about 300 ms after you press rather than straight away.

## BLE protocol

The device advertises a custom GATT service alongside the standard HID keyboard service:

|                            | UUID                                   |
| -------------------------- | -------------------------------------- |
| **Data Service**           | `4c41555a-4465-7669-6365-000000000001` |
| RX Characteristic (write)  | `4c41555a-4465-7669-6365-000000000002` |
| TX Characteristic (notify) | `4c41555a-4465-7669-6365-000000000003` |
| CTRL Characteristic (write) | `4c41555a-4465-7669-6365-000000000005` |
| **HID Service**            | `00001812-0000-1000-8000-00805f9b34fb` |

JSON payload format (written to RX):

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": true }
```

Fields: `s` = session %, `sr` = session reset (minutes), `w` = weekly %, `wr` = weekly reset (minutes), `st` = status, `ok` = success flag.

The CTRL characteristic carries the OTA control commands; it is writable only over a
bonded and encrypted link, and every command is answered on the TX characteristic. See
[Firmware updates](#firmware-updates-hybrid-ota) and
[`design/ota-hybrid/DESIGN.md`](design/ota-hybrid/DESIGN.md).

## Firmware updates (hybrid OTA)

The device can update its own firmware over WiFi, triggered over the BLE link you
already have — no USB cable. BLE is only the control path; the binary transfer
happens over WiFi, and the radio is brought up only for the transfer.

```bash
# from the repo root, with the daemon installed
python daemon/ota_flash.py \
    --firmware firmware/.pio/build/waveshare_amoled_216/firmware.bin \
    --ssid "<your wifi>" --pass "<password>"
```

The helper:

1. Stops the running daemon so the single BLE connection is free, and restarts it
   when it finishes (`--keep-daemon` skips this).
2. Connects, checks the device reports `board = "waveshare_amoled_216"`, and
   provisions the WiFi credentials over the CTRL characteristic.
3. Puts the device into OTA mode and reads back its IP.
4. Uploads the binary with `espota` over WiFi.
5. Switches OTA mode back off; the device reboots into the new slot.

Optional shared password for the transfer: `--ota-password <pw>`. The config file
documents `ota_ssid` / `ota_wifi_password` / `ota_password` as defaults the CLI
flags override (see `daemon/config.example`).

The host must be on the same network as the device, since the upload is a direct
WiFi connection. If the new image fails to boot repeatedly, the firmware switches
back to the previous slot on its own after three attempts. On Windows, quit the
tray app before flashing — see [`daemon/README-windows.md`](daemon/README-windows.md).

Adding the CTRL characteristic changes the GATT table; if your host caches GATT
per device, re-pair the device once (hold PWR ~3 s then release) so it sees the
new characteristic.

## Development

<img src="assets/readme/crab.gif" width="120" align="right" alt="">

- **Desktop simulator** — iterate on the UI without hardware: an SDL2 window
  runs the full firmware loop with scenario playback (`pio run -d firmware -e
sim`, then `cd firmware && .pio/build/sim/program`). See
  [`SIM-USAGE.md`](SIM-USAGE.md) for controls, scenarios, and headless
  screenshots.
- **Splash animations** — Anthropic's official Clawd sprites, archived with
  provenance notes in [`research/clawd-official/`](research/clawd-official/);
  `node tools/convert_official_clawd.js` regenerates
  `firmware/src/splash_animations.h`. See [`tools/README.md`](tools/README.md).
- **Icons** — Lucide PNGs convert to LVGL C arrays with
  `tools/png_to_lvgl.js`. See [`tools/README.md`](tools/README.md).
- **Fonts** — the pre-compiled LVGL fonts and the LVGL-9 patching they need:
  [`docs/fonts.md`](docs/fonts.md).

## Credits

- Pixel-art Clawd animations are Anthropic's official mascot art (claude.ai/code, Claude Code desktop), archived and converted by the tooling in `tools/` and `research/clawd-official/`.
- OpenCode logo and wordmark pixel grids from [anomalyco/opencode](https://github.com/anomalyco/opencode) (MIT); IBM Plex Mono from [IBM/plex](https://github.com/IBM/plex) (SIL OFL 1.1).
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for bluetooth and battery UI glyphs.
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing warning below.

## Licensing gray area warning

The software in this repository uses and adheres to the Anthropic brand guidelines and uses the same proprietary fonts that Anthropic has a license for but this software uses without permission as well as using assets from Anthropic such as the copyrighted Clawd mascot so even though the code in this repo is non-proprietary I will not license it myself under a copyleft license since this repo includes proprietary fonts and copyrighted assets. Please be aware of this if you fork or copy the code from this repo. **You have been warned!**
