#pragma once

// Native desktop simulator "board" — an SDL2 window standing in for an AMOLED
// so shared code (main.cpp, ui.cpp, splash.cpp) can be developed without
// hardware. No pins here, just geometry and the key map.
//
//   mouse / left-click   touch (tap = next screen)
//   space                play/pause scenario playback
//   left / right         step one scenario state (pauses playback)
//   1..9                 jump to scenario state N (pauses playback)
//   d                    toggle BLE connected/disconnected
//   b  (tap)             PRIMARY: previous screen (next on 1-button boards)
//   b  (hold 300ms)      PRIMARY: HID Space held down (voice-mode PTT)
//   n  (tap)             SECONDARY: next screen
//   n  (hold 300ms)      SECONDARY: HID Shift+Tab held down (mode toggle)
//   p                    PWR (short: splash scene / brightness; hold ~3s +
//                        release = pair)
//   c                    toggle charging       - / =   battery down / up 5%
//   s                    save screenshot BMP to the current directory
//   esc / window close   quit
//
// Scenario: sim/scenario.jsonl (relative to the firmware/ dir), overridable
// with SIM_SCENARIO=<path>. One JSON object per line — the daemon payload
// plus optional "name" and "hold_ms" (default 3000). Lines starting with #
// are comments. Missing file → a small built-in state list.
// sim/scenario-opencode.jsonl interleaves the Claude beats with OpenCode ones
// (tagged "k":"oc"), which grows the screen cycle to four screens.
//
// Geometry is 480x480 (the panel this fork targets). LCD_WIDTH / LCD_HEIGHT /
// BOARD_NAME can still be overridden from build_flags. SIM_BUTTONS=1 at run
// time emulates a board with no SECONDARY button.
//
// Headless / CI: SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=<ms> saves a
// screenshot (SIM_AUTOSHOT_PATH, default sim-autoshot.bmp) after <ms> and
// exits. SIM_START_SCREEN=splash|usage|oc_splash|oc_usage jumps straight to a
// screen once the first payloads have landed (or after 1.5 s), which is what
// makes a single autoshot deterministic.

#ifndef BOARD_NAME
#define BOARD_NAME  "Simulator 480x480"
#endif
#ifndef LCD_WIDTH
#define LCD_WIDTH   480
#endif
#ifndef LCD_HEIGHT
#define LCD_HEIGHT  480
#endif
