// Native entry point — stands in for the Arduino runtime. loop()'s own
// delay(5) paces the loop the same way it does on hardware.
#include <Arduino.h>
#include "sim_platform.h"

int main(void) {
    printf(
        "Clawdmeter simulator\n"
        "  mouse          touch (tap = next screen)\n"
        "  space          play/pause scenario    left/right step    1-9 jump\n"
        "  d              toggle BLE link\n"
        "  b tap / hold   PRIMARY: prev screen (next on 1-button) / HID Space\n"
        "  n tap / hold   SECONDARY: next screen / HID Shift+Tab\n"
        "  p              PWR (short: scene/brightness, hold ~3s: pair)\n"
        "  c              toggle charging            - / =  battery down/up\n"
        "  s              screenshot                 esc  quit\n\n");
    setup();
    while (!sim_should_quit()) {
        sim_pump();
        loop();
    }
    return 0;
}
