#pragma once
#include "data.h"
#include "ble.h"
#include "oc_data.h"
#include "pf_data.h"

// Navigation cycle (SPEC.md §6): Clawd splash → Claude usage → OpenCode
// splash → OpenCode usage → BVC portfolio → back to the Clawd splash. The two
// OpenCode screens stay out of the cycle until the first OpenCode payload
// lands, and the portfolio until the first "k":"pf" one, so a device that
// never receives either behaves exactly as before.
enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_OC_SPLASH,
    SCREEN_OC_USAGE,
    SCREEN_PORTFOLIO,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_opencode(const OcData* data);
void ui_update_portfolio(const PfData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_next_screen(void);
void ui_prev_screen(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
