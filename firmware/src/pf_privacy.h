#pragma once
#include <Arduino.h>

// Portfolio private mode (privacy spec §6, §7).
//
// One bool, persisted in NVS under "pf_priv" in the "clawdmeter" namespace —
// the same namespace and the same lifetime as `brt_idx`. It survives a screen
// change, the idle fade and a reboot, because the risk this mode answers is
// exactly the one that happens in public: the device gets unplugged, rebooted
// and woken up there. Default is OFF — the user asked for that screen to see
// their numbers.
//
// The payload never changes: this is visibility, not transport.
void pf_privacy_init(void);            // read NVS, called once at boot
void pf_privacy_toggle(void);          // invert, persist, arm the 1.5 s notice
bool pf_privacy_get(void);             // current mode
bool pf_privacy_notice(void);          // true while the transient notice shows
