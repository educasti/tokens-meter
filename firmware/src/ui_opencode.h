#pragma once
#include <lvgl.h>
#include "oc_data.h"

// OpenCode usage screen (SPEC.md §4, layout in Annex A). Own container, own
// palette; ui.cpp owns the navigation, the page indicator and the battery.
void oc_usage_init(lv_obj_t *parent);   // full-size container, hidden
void oc_usage_show(void);
void oc_usage_hide(void);
lv_obj_t *oc_usage_get_root(void);      // ui.cpp attaches the tap handler
void oc_usage_update(const OcData *d);  // stores a copy, stamps millis(), redraws
void oc_usage_set_ble(bool connected);
void oc_usage_tick(void);               // status blink, stale detection, "last activity" ageing
bool oc_has_data(void);                 // a valid payload has arrived since boot
