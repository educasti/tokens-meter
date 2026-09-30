#pragma once
#include <lvgl.h>
#include "pf_data.h"

// BVC portfolio screen (portfolio spec §2, §4, §7). Own container, own palette,
// IBM Plex Mono — the same shape as the OpenCode usage screen. ui.cpp owns the
// navigation, the page indicator and the battery.
//
// Private mode (privacy spec §1-§5) is a visibility switch inside this file:
// the payload is identical either way, and the flag lives in pf_privacy.h.
void pf_usage_init(lv_obj_t *parent);   // full-size container, hidden
void pf_usage_show(void);
void pf_usage_hide(void);
lv_obj_t *pf_usage_get_root(void);      // ui.cpp attaches the tap handler
void pf_usage_update(const PfData *d);  // stores a copy, stamps millis(), redraws
void pf_usage_set_ble(bool connected);
void pf_usage_tick(void);               // mode changes + the status line's own clocks
bool pf_has_data(void);                 // a valid payload has arrived since boot
