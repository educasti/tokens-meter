#include "../../hal/board_caps.h"
#include "board.h"
#include <stdlib.h>

// B and N keys stand in for BOOT + the secondary button. SIM_BUTTONS=1 makes
// the sim behave like a one-button board (e.g. the LCD-4), which changes which
// direction the PRIMARY tap walks the screen cycle in. Read from the
// environment rather than build_flags so the same binary can test both.
static int sim_button_count(void) {
    const char* v = getenv("SIM_BUTTONS");
    return (v && atoi(v) == 1) ? 1 : 2;
}

static BoardCaps caps = {
    .name = BOARD_NAME,
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .button_count = 2,      // patched below on first use
    .has_rotation = false,
    .has_battery = true,    // fake battery, adjustable with -/=
    .has_imu = false,
};

const BoardCaps& board_caps(void) {
    caps.button_count = (uint8_t)sim_button_count();
    return caps;
}
