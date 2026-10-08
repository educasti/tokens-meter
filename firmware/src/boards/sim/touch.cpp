#include "board.h"
#include "../../hal/touch_hal.h"
#include <Arduino.h>
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

void touch_hal_init(void) {}

// Gesto sintético headless para probar el Command Center sin ratón real
// (con SDL_VIDEODRIVER=dummy no hay cursor que mover). SIM_SWIPE inyecta
// una secuencia como si un dedo la hiciera, solo en el sim:
//
//   SIM_SWIPE="down@2000,up@4000,tap@5500"
//
// Cada gesto es tipo[@ms_origen]; el @ms es opcional (por defecto 2000 ms
// el primero y +1500 ms cada siguiente). Tipos:
//   down = swipe hacia abajo desde el borde superior (abre el overlay)
//   up   = swipe hacia arriba (lo cierra)
//   tap  = toque corto en el centro (lo cierra)
// Mientras un gesto está activo manda sobre el ratón; fuera de ellos el
// ratón real sigue funcionando igual que antes.
#define SWIPE_MAX  8

struct SwipeStep {
    bool     is_tap;     // true = toque quieto, false = arrastre
    uint32_t start_ms;   // origen del gesto
    uint32_t dur_ms;     // duración presionado
    int16_t  x0, y0;     // punto de apoyo
    int16_t  x1, y1;     // punto de suelta (igual al de apoyo en tap)
};

static SwipeStep swipe_list[SWIPE_MAX];
static int       swipe_n = -1;   // -1 = aún sin leer SIM_SWIPE

// El movimiento se interpola por tiempo (~1 px/ms), así el vector por
// lectura supera el umbral de gesto de LVGL vaya como vaya de rápido el
// loop del sim.
#define SWIPE_DRAG_MS  350
#define SWIPE_TAP_MS   120

static void swipe_parse(void) {
    swipe_n = 0;
    const char* env = getenv("SIM_SWIPE");
    if (!env || !*env) return;
    char buf[160];
    strlcpy(buf, env, sizeof(buf));
    uint32_t fallback_ms = 2000;
    char* tok = strtok(buf, ",");
    while (tok && swipe_n < SWIPE_MAX) {
        // Separa el "@ms" opcional del nombre del gesto.
        uint32_t at = fallback_ms;
        char* at_sign = strchr(tok, '@');
        if (at_sign) {
            *at_sign = '\0';
            at = (uint32_t)atol(at_sign + 1);
        }
        SwipeStep* s = &swipe_list[swipe_n];
        s->start_ms = at;
        if (strcmp(tok, "down") == 0) {
            // Borde superior (y=30 < SYS_EDGE_Y=60) hacia abajo.
            s->is_tap = false; s->dur_ms = SWIPE_DRAG_MS;
            s->x0 = LCD_WIDTH / 2; s->y0 = 30;
            s->x1 = LCD_WIDTH / 2; s->y1 = 320;
        } else if (strcmp(tok, "up") == 0) {
            s->is_tap = false; s->dur_ms = SWIPE_DRAG_MS;
            s->x0 = LCD_WIDTH / 2; s->y0 = 400;
            s->x1 = LCD_WIDTH / 2; s->y1 = 120;
        } else if (strcmp(tok, "tap") == 0) {
            s->is_tap = true; s->dur_ms = SWIPE_TAP_MS;
            s->x0 = LCD_WIDTH / 2; s->y0 = LCD_HEIGHT / 2;
            s->x1 = s->x0;         s->y1 = s->y0;
        } else {
            // Token desconocido: se ignora sin romper el resto.
            tok = strtok(NULL, ",");
            continue;
        }
        swipe_n++;
        fallback_ms = at + 1500;
        tok = strtok(NULL, ",");
    }
}

// Si hay un gesto activo ahora, rellena x/y/pressed y devuelve true.
static bool swipe_poll(uint16_t* x, uint16_t* y, bool* pressed) {
    if (swipe_n < 0) swipe_parse();
    if (swipe_n == 0) return false;
    uint32_t now = millis();
    for (int i = 0; i < swipe_n; i++) {
        const SwipeStep* s = &swipe_list[i];
        if (now < s->start_ms || now >= s->start_ms + s->dur_ms) continue;
        uint32_t el = now - s->start_ms;
        int32_t px = s->x0 + (int32_t)(s->x1 - s->x0) * (int32_t)el / (int32_t)s->dur_ms;
        int32_t py = s->y0 + (int32_t)(s->y1 - s->y0) * (int32_t)el / (int32_t)s->dur_ms;
        *x = (uint16_t)px;
        *y = (uint16_t)py;
        *pressed = true;
        return true;
    }
    return false;
}

// Mouse position + left button = one finger. Events are pumped every loop
// by sim_main.cpp, so SDL_GetMouseState is always fresh here.
void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (swipe_poll(x, y, pressed)) return;
    int mx, my;
    uint32_t b = SDL_GetMouseState(&mx, &my);
    if (mx < 0) mx = 0;
    if (mx >= LCD_WIDTH) mx = LCD_WIDTH - 1;
    if (my < 0) my = 0;
    if (my >= LCD_HEIGHT) my = LCD_HEIGHT - 1;
    *x = (uint16_t)mx;
    *y = (uint16_t)my;
    *pressed = (b & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
}
