#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "ota.h"
#include "ota_pull.h"
#include "splash.h"
#include "oc_splash.h"
#include "oc_data.h"
#include "pf_data.h"
#include "pf_privacy.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};
static OcData    oc = {};
static PfData    pf = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;
    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    // Hybrid OTA (§4): load WiFi creds and arm/roll back the boot-verify
    // counter as early as possible — a failed slot should reboot before any
    // heavy display/BLE bring-up.
    ota_init();
    // Pull OTA shares the "otah" namespace and the WiFi owner; load its schedule
    // and start the worker right after the hybrid path is armed.
    ota_pull_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle
    pf_privacy_init();  // load the saved portfolio private-mode flag (default off)

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

// Press longer than this and the button stops being navigation and becomes a
// held HID key (SPEC.md §6). The cost of the change is that both keys now go
// out ~300 ms later than a direct press-and-hold used to.
#define BUTTON_HOLD_MS 300

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    ota_tick();
    ota_pull_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    // No-op unless the OpenCode splash owns the shared canvas (oc_splash.h).
    oc_splash_tick();
    splash_mascot_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    // Tap vs hold (SPEC.md §6): a press released before HOLD_MS navigates the
    // screen cycle; a press still down at HOLD_MS sends the HID key and holds
    // it until release, so voice-mode PTT and the mode toggle keep working
    // while a tap can no longer be mistaken for a keypress.
    //   PRIMARY   → tap: previous screen (next on single-button boards)
    //               hold: HID Space held down  (Claude Code voice-mode PTT)
    //   SECONDARY → tap: next screen
    //               hold: HID Shift+Tab held down (mode toggle; 2-button boards)
    //   PWR       → on the Clawd splash: next animation; on the OpenCode
    //               splash: next scene; on the portfolio: toggle its private
    //               mode; elsewhere: cycle brightness
    //   touch      → next screen (ui.cpp's global_click_cb)
    //   hold PWR ~3s + release → pairing, unchanged
    // power_hal_pwr_pressed() is a *release* edge that only fires under
    // PWR_LONG_MS (1.5 s): between 1.5 s and 3 s the release cancels the
    // pending pair gesture and does nothing else, so no action may be hung
    // there. First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); a swallowed press does nothing at all until
    // release. Activity bookkeeping happens inside idle_consume_wake_press, so
    // a navigation needs no separate idle_note_activity() call.
    {
        // Per-button tap/hold state. The loop is edge-driven for the two edges
        // and level-driven for the hold deadline, so a press is classified
        // exactly once no matter how the loop's timing lines up with it.
        struct btn_state {
            bool     down;            // currently held
            bool     wake_swallowed;  // this press was eaten waking the panel
            bool     hid_sent;        // HID key already sent for this press
            uint32_t down_ms;
            uint8_t  hid_key, hid_mod;
            int      nav_dir;         // tap action: -1 previous, +1 next
        };
        static btn_state primary   = {};
        static btn_state secondary = {};
        primary.hid_key   = 0x2C;    // HID Space, no mods
        primary.hid_mod   = 0x00;
        secondary.hid_key = 0x2B;    // HID Tab
        secondary.hid_mod = 0x02;    // + LEFT_SHIFT

        // On a one-button board PRIMARY is the only way forward, so it walks
        // the cycle in the same direction as SECONDARY.
        primary.nav_dir = (board_caps().button_count >= 2) ? -1 : +1;
        secondary.nav_dir = +1;

        auto tick_button = [](btn_state& b, bool now) {
            if (now != b.down) {
                if (now) {
                    // Press edge: a wake is consumed first, and then this press
                    // does nothing until release.
                    b.wake_swallowed = idle_consume_wake_press();
                    b.hid_sent = false;
                    b.down_ms = millis();
                } else {
                    // Release edge: only a press that already sent a key needs
                    // the release report; a tap navigates, a swallowed press
                    // does nothing.
                    if (b.hid_sent) {
                        ble_keyboard_release();
                    } else if (!b.wake_swallowed) {
                        if (b.nav_dir < 0) ui_prev_screen();
                        else               ui_next_screen();
                    }
                }
                b.down = now;
                return;
            }
            // Still held: the HID key goes down once the tap window closes,
            // and stays down (repeat is not needed — the host's key-repeat
            // handles the rest) until release.
            if (now && !b.wake_swallowed && !b.hid_sent &&
                (millis() - b.down_ms) >= BUTTON_HOLD_MS) {
                b.hid_sent = true;
                ble_keyboard_press(b.hid_key, b.hid_mod);
            }
        };

        tick_button(primary, input_hal_is_held(INPUT_BTN_PRIMARY));
        if (board_caps().button_count >= 2) {
            tick_button(secondary, input_hal_is_held(INPUT_BTN_SECONDARY));
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // Each screen with something of its own cycles it; the rest get
                // the brightness, which is global and persistent anyway.
                switch (ui_get_current_screen()) {
                case SCREEN_SPLASH:     splash_next(); break;
                case SCREEN_OC_SPLASH:  oc_splash_next_scene(); break;
                case SCREEN_PORTFOLIO:  pf_privacy_toggle(); break;
                default:                brightness_cycle(); break;
                }
            }
        }

        pair_tick();
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        // Route on the payload tag (SPEC.md §8): an OpenCode beat never touches
        // the Claude path — no usage-rate sample, no chime, no ui_update — a
        // portfolio beat never touches either, and vice versa. The two
        // is_payload() calls are substring tests, so they cost nothing next to
        // the parse either way.
        const char* msg = ble_get_data();
        if (oc_is_payload(msg)) {
            if (oc_parse(msg, &oc)) {
                ui_update_opencode(&oc);
                ble_send_ack();
                ota_confirm();   // valid owner payload → boot is good (§4)
            } else {
                ble_send_nack();
            }
        } else if (pf_is_payload(msg)) {
            if (pf_parse(msg, &pf)) {
                ui_update_portfolio(&pf);
                ble_send_ack();
                ota_confirm();
            } else {
                ble_send_nack();
            }
        } else if (parse_json(msg, &usage)) {
            int g_before = usage_rate_group();
            bool session_reset = usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            // 5-hour session limit refilled → chime so the user knows they can
            // use Claude again (no-op on boards without a buzzer). Gated on the
            // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_play_reset();
            }
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
            ota_confirm();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
