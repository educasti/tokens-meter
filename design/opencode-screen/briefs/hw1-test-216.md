# HW1 — Hardware test on the Waveshare AMOLED-2.16 (S3)

Repo: /Users/educasti/Projects/Personal/tokens-meter, branch `feat/opencode-screens` (the OpenCode screens are implemented and reviewed in the simulator; read `design/opencode-screen/SPEC.md` §4–§7 to know what each screen should look like, and `CLAUDE.md` sections "Build / flash", "QA your own UI changes", "Screens and navigation").

The board is connected on **/dev/cu.usbmodem101**. The user is watching and will press buttons / tap the screen when asked.

## Rules
- Do **not** modify any source file in the repo. The only repo path you may create is `daemon/.venv/` (check it is gitignored first; if not, use `/tmp/tm-venv` instead).
- Do **not** touch the installed LaunchAgent (`~/Library/LaunchAgents/com.user.claude-usage-daemon.plist`) or the other checkout at `~/Projects/Personal/clawd-meter`. (It is currently not running: its venv's httpx fails with an SSL `FileNotFoundError` — just report that, don't fix it.)
- Never print or log the OpenCode Go key or the Claude token. Do not `cat` `auth.json`, `service.json` or the Keychain.
- Do not commit.

## Steps
1. **Flash:** `pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem101`. Report the result.
2. **Boot log:** read the serial port for ~8 s after reset (use pio's Python + pyserial, e.g. `~/.platformio/penv/bin/python -c "import serial,time; ..."` at 115200) and report the relevant lines (e.g. "Dashboard ready", BLE state). Close the port afterwards; `screenshot.sh` needs it.
3. **Daemon config:** back up `~/.config/claude-usage-monitor/config` to `~/.config/claude-usage-monitor/config.bak-oc-test`, then add a line `opencode = on` if it is not already there.
4. **Daemon from this branch (foreground, temporary):**
   - Create the venv: `python3 -m venv daemon/.venv && daemon/.venv/bin/pip install bleak httpx`.
   - Run `daemon/.venv/bin/python daemon/claude_usage_daemon.py > /tmp/tm-daemon.log 2>&1 &`, remember the PID, and watch the log for 2–3 minutes.
   - Expected: it connects to "Clawdmeter", writes the Claude payload, and ~60 s in logs `OpenCode: api 5h …% week …% month …% 7d …k`.
   - If macOS asks for Bluetooth permission or BLE fails with a permission error, stop and tell the user exactly what to allow.
   - Report the relevant log lines (never any secret).
5. **Screens on the real panel:** tell the user, in your pane, to tap the screen to walk the cycle: Clawd splash → Claude → OpenCode splash → OpenCode usage. Page dots should show 4 dots once the first OpenCode payload has landed.
   - After each step take a screenshot with `./screenshot.sh /tmp/hw-shots/<n>-<name>.png /dev/cu.usbmodem101` (create the dir) and describe what you see.
   - Also ask the user to try: a short tap on the right side button (next screen), a short tap on the left button (previous screen), holding the left button (should type spaces on the Mac), and PWR on the OpenCode splash (next scene: typeon → assemble → scanner).
   - Record what the user reports.
6. **Compare** each screenshot with SPEC.md §4/§5 and Annex A. List any difference: position, colour, text or truncation. Hardware-only issues (panel offsets, rounding, tearing) are what we are looking for.
7. **Leave the daemon running** (don't kill it) so the user can keep looking; give its PID and the command to stop it (`kill <pid>`), plus how to restore the config (`mv ~/.config/claude-usage-monitor/config.bak-oc-test ~/.config/claude-usage-monitor/config`).

## Deliverable
Write `/Users/educasti/Projects/Personal/tokens-meter/design/opencode-screen/research/hw1-test-216.md` with: flash result, boot log excerpt, daemon log excerpt (secrets-free), the list of screenshots with a one-line description each, button test results, and the differences found (or "none"). Reply `DONE HW1` + a one-line verdict.
