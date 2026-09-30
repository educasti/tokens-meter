#include "pf_privacy.h"
#include <Preferences.h>
#include <Arduino.h>

// Same NVS namespace and the same key style as brightness.cpp's `brt_idx`:
// both are local user preferences that must outlive a reflash-free reboot.
#define PF_NVS_NS   "clawdmeter"
#define PF_NVS_KEY  "pf_priv"

// The transient "modo privado" / "modo normal" line lasts exactly as long as
// the page dots, for the same reason: the user has a "PWR = brightness"
// habit, and this is what tells them in the act why the screen changed.
#define PF_NOTICE_MS 1500u

// Default off: the first boot has to look like every other boot.
static bool     pf_private = false;
static uint32_t notice_ms  = 0;

void pf_privacy_init(void) {
    Preferences prefs;
    prefs.begin(PF_NVS_NS, true);
    pf_private = prefs.getUChar(PF_NVS_KEY, 0) != 0;
    prefs.end();
    Serial.printf("Portfolio privacy: %s\n", pf_private ? "privado" : "normal");
}

void pf_privacy_toggle(void) {
    pf_private = !pf_private;

    Preferences prefs;
    prefs.begin(PF_NVS_NS, false);
    prefs.putUChar(PF_NVS_KEY, pf_private ? 1 : 0);
    prefs.end();

    notice_ms = millis();
    Serial.printf("Portfolio privacy toggled: %s\n", pf_private ? "privado" : "normal");
}

bool pf_privacy_get(void) {
    return pf_private;
}

bool pf_privacy_notice(void) {
    return (uint32_t)(millis() - notice_ms) < PF_NOTICE_MS;
}
