#include "oc_splash.h"
#include "splash.h"
#include "splash_geometry.h"
#include <Arduino.h>
#include <string.h>

// Splash de OpenCode, portado del prototipo aprobado
// design/opencode-screen/oc-splash.js: mismas escenas, mismos tiempos en
// milisegundos, mismos colores, misma tabla de fotogramas del escáner. El
// prototipo dibuja en un lienzo con alfa; aquí cada color dibujado es la mezcla
// rgba del prototipo sobre el escenario negro, convertida en una entrada de
// paleta RGB565 una sola vez, al componer, para que el módulo de escena siga
// siendo tan barato como las animaciones de Clawd (un búfer de celdas 60×60 más
// una paleta pequeña) y la C6 no reciba búferes ni objetos LVGL nuevos.
//
// El temporizado de fotogramas sigue a splash.cpp: el tiempo transcurrido se
// deduce de millis() y solo se empuja un fotograma cuando el resultado
// compuesto ha cambiado de verdad.

#define GRID  SPLASH_GRID

// ─── Paleta ─────────────────────────────────────────────────────────────────
// Los valores de celda son índices en `pal[]`, y la ruta de render de la C6
// encuentra el rectángulo sucio comparando los *valores* de las celdas entre
// fotogramas — así que un índice debe conservar su color mientras se reutilice.
// La tabla es por eso direccionable por contenido y se construye una sola vez
// por escena/estado de ánimo (pal_build) y nunca se reordena: dos colores no
// pueden intercambiar índices, y un color nuevo solo se añade al final. Se
// pre-registra cada color que una escena puede dibujar, así que la
// correspondencia es constante durante toda la escena y la comparación sigue
// siendo exacta.
#define PAL_BG  0            // pal[0] es el escenario negro
#define PAL_MAX SPLASH_PALETTE_MAX

static uint8_t  cells[GRID * GRID];   // 3,6 KB de RAM estática, igual que en splash.cpp
static uint16_t pal[PAL_MAX];
static uint8_t  pal_count;

// Colores de marca + interfaz (spec §2.3 / las leyendas de rejilla del prototipo).
#define COL_MARK      0xF1ECECu   // 'O' / 'C' — la marca y la cara del wordmark
#define COL_WORD      0xB7B1B1u   // 'B' — el cuerpo del wordmark
#define COL_INNER     0x4B4646u   // 'i' / 'A' — interior de la marca, recortes del wordmark
#define COL_INNER_ALT 0x5A5858u   // al respirar el ensamblado el interior se aclara
#define COL_LIMITED   0xE06C75u   // rojo de error del spec — parpadeo de limited

// ─── Estado del módulo ──────────────────────────────────────────────────────
enum { SCENE_TYPEON, SCENE_ASSEMBLE, SCENE_SCANNER, SCENE_COUNT };

static bool      active = false;
static oc_mood_t mood   = OC_MOOD_IDLE;
static uint8_t   scene  = SCENE_TYPEON;
static bool      manual = false;    // hay una escena forzada por el botón PWR
static bool      once   = false;    // ensamblado de un tiro → vuelta a la escena automática
static uint32_t  scene_ms = 0;      // intro de escena, según el scene.time del prototipo
static uint32_t  next_ms  = 0;      // instante más temprano en que el fotograma puede cambiar otra vez
static uint32_t  frame_key = 0;     // identidad del último fotograma empujado
static bool      drawn = false;

// ─── Aritmética de color ────────────────────────────────────────────────────
// Mismo truncado 5/6/5 que lv_color_to_u16() de LVGL, es decir, el orden de
// bytes en que se escriben las paletas de splash_animations.h.
static inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

static inline uint16_t hex565(uint32_t rgb) {
    return rgb565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Una mezcla rgba sobre el escenario negro, precalculada: `pct` es el alfa en
// porcentaje, así que el fotograma entero es una paleta de colores RGB565
// opacos.
static inline uint16_t shade(uint32_t rgb, int pct) {
    return rgb565(((rgb >> 16) & 0xFF) * pct / 100,
                  ((rgb >>  8) & 0xFF) * pct / 100,
                  ( (rgb      ) & 0xFF) * pct / 100);
}

// El adjustBrightness(base, 1.15) del prototipo, con suelo por canal.
static inline uint32_t brighten(uint32_t rgb) {
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t v = (rgb >> (16 - 8 * i)) & 0xFF;
        v = v * 115 / 100;
        if (v > 255) v = 255;
        out |= v << (16 - 8 * i);
    }
    return out;
}

static void pal_reset(void) {
    pal[PAL_BG] = 0x0000;
    pal_count   = PAL_BG + 1;
}

// Interioriza un color: mismo color → mismo índice; color nuevo → un índice
// nuevo al final. Nunca reordena ni reasigna.
static uint8_t pal_get(uint16_t color) {
    for (uint8_t i = 0; i < pal_count; i++)
        if (pal[i] == color) return i;
    if (pal_count >= PAL_MAX) return PAL_BG;   // pal_build() llega antes
    pal[pal_count] = color;
    return pal_count++;
}

static inline void put(int x, int y, uint16_t color) {
    if (x < 0 || x >= GRID || y < 0 || y >= GRID) return;
    cells[y * GRID + x] = pal_get(color);
}

// ─── Arte de origen (material oficial de OpenCode, research §2.3) ───────────
static const char *const OC_MARK[5] = {
    "OOOO",
    "O..O",
    "OiiO",
    "OiiO",
    "OOOO",
};

#define WORDMARK_H 7
static const char *const OC_WORDMARK[WORDMARK_H] = {
    ".................................C.....",
    "BBBB.BBBB.BBBB.BBB..CCCC.CCCC.CCCC.CCCC",
    "B..B.B..B.B..B.B..B.C....C..C.C..C.C..C",
    "BAAB.BAAB.BBBB.BAAB.CAAA.CAAC.CAAC.CCCC",
    "BAAB.BAAB.BAAA.BAAB.CAAA.CAAC.CAAC.CAAA",
    "BBBB.BBBB.BBBB.BAAB.CCCC.CCCC.CCCC.CCCC",
    ".....B.................................",
};

// Leyenda de rejilla → color; 0 significa dejar la celda en negro.
static uint16_t grid_color(char ch) {
    switch (ch) {
        case 'O': case 'C': return hex565(COL_MARK);
        case 'B':         return hex565(COL_WORD);
        case 'i': case 'A': return hex565(COL_INNER);
        default:          return 0;
    }
}

// ─── Estados de ánimo ───────────────────────────────────────────────────────
static const uint32_t MOOD_BASE[OC_MOOD_LIMITED + 1] = {
    0xFAB283u,   // idle    — melocotón
    0xFAB283u,   // active
    0xFAB283u,   // busy
    0xF5A742u,   // near    — ámbar
    0xE06C75u,   // limited — rojo
};
// Solo busy levanta la cabeza del escáner; los demás reutilizan su base.
static const uint32_t MOOD_HEAD[OC_MOOD_LIMITED + 1] = {
    0xFAB283u, 0xFAB283u, 0xFFC09Fu, 0xF5A742u, 0xE06C75u,
};

static inline uint8_t auto_scene(void) {
    return (mood == OC_MOOD_IDLE) ? SCENE_TYPEON : SCENE_SCANNER;
}

// ─── Escena: typeon ─────────────────────────────────────────────────────────
// El wordmark se teclea letra a letra con un cursor melocotón que parpadea. El
// cursor toma el color del estado de ánimo, así que esta escena también lo
// comunica.
#define TYPEON_LEAD_MS   200
#define TYPEON_CURSOR_MS 500
static const uint16_t TYPEON_DELAY[8] = { 60, 60, 60, 150, 60, 60, 250, 60 };
static const uint8_t  TYPEON_COL[8]   = {  0,  5, 10, 15, 20, 25, 30, 35 };

static uint32_t compose_typeon(uint32_t elapsed, uint32_t key) {
    if (elapsed < TYPEON_LEAD_MS) return key;   // todavía en negro
    const uint32_t t = elapsed - TYPEON_LEAD_MS;

    uint8_t drawn_letters = 0;
    uint32_t delay = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (delay <= t) drawn_letters = i + 1;
        delay += TYPEON_DELAY[i];
    }

    for (uint8_t i = 0; i < drawn_letters; i++) {
        const int lx = TYPEON_COL[i];
        for (int r = 0; r < WORDMARK_H; r++) {
            for (int c = 0; c < 4; c++) {
                const uint16_t col = grid_color(OC_WORDMARK[r][lx + c]);
                if (col) put(10 + lx + c, 26 + r, col);
            }
        }
    }

    const uint32_t phase = (t / TYPEON_CURSOR_MS) % 2;
    if (phase == 0) {
        const int cx = 10 + (drawn_letters ? TYPEON_COL[drawn_letters - 1] + 4 : 0);
        const uint16_t col = shade(MOOD_BASE[mood], 100);
        for (int r = 27; r <= 31; r++) put(cx, r, col);
    }

    // 0 mientras la entrada sigue en negro; después 1 + (letras, fase).
    return key | ((1u + drawn_letters * 2u + phase) << 8);
}

// ─── Escena: assemble ───────────────────────────────────────────────────────
// La marca aparece celda a celda, en sentido horario, y luego el interior
// respira.
#define ASM_SCALE     6
#define ASM_X         18
#define ASM_Y         15
#define ASM_STEP_MS   40
#define ASM_HOLD_MS   120
#define ASM_ALT_MS    800
// Duración del tiro único: intro del anillo + dos respiraciones completas.
#define ASM_ONCE_MS   ((40 * 14) + ASM_HOLD_MS + 2 * ASM_ALT_MS)

static const uint8_t ASM_RING[14][2] = {   // arriba, derecha, abajo, izquierda
    {0,0},{1,0},{2,0},{3,0},
    {3,1},{3,2},{3,3},{3,4},
    {2,4},{1,4},{0,4},
    {0,3},{0,2},{0,1},
};
// El interior 2×2 de la marca, compartido por la escena assemble y el parpadeo
// limited.
static const uint8_t MARK_INNER[4][2] = { {1,2},{2,2},{1,3},{2,3} };

// Una celda de la marca ampliada a un cuadrado de ASM_SCALE.
static void asm_block(const uint8_t cell[2], uint16_t color) {
    const uint8_t idx = pal_get(color);
    for (int sr = 0; sr < ASM_SCALE; sr++) {
        const int y = ASM_Y + cell[1] * ASM_SCALE + sr;
        if (y < 0 || y >= GRID) continue;
        for (int sc = 0; sc < ASM_SCALE; sc++) {
            const int x = ASM_X + cell[0] * ASM_SCALE + sc;
            if (x >= 0 && x < GRID) cells[y * GRID + x] = idx;
        }
    }
}

static uint32_t compose_assemble(uint32_t elapsed, uint32_t key) {
    const uint32_t intro = ASM_STEP_MS * 14;   // 560 ms de celdas del anillo
    if (elapsed < intro) {
        const int upto = (int)(elapsed / ASM_STEP_MS);      // 0..13
        for (int i = 0; i <= upto; i++)
            asm_block(ASM_RING[i], hex565(COL_MARK));
        return key | ((uint32_t)(upto + 1) << 8);
    }

    for (int i = 0; i < 14; i++) asm_block(ASM_RING[i], hex565(COL_MARK));

    const uint32_t t = elapsed - intro;
    uint16_t inner = hex565(COL_INNER);
    if (t >= ASM_HOLD_MS && ((t - ASM_HOLD_MS) / ASM_ALT_MS) % 2)
        inner = hex565(COL_INNER_ALT);
    for (int i = 0; i < 4; i++) asm_block(MARK_INNER[i], inner);

    return key | (inner == hex565(COL_INNER) ? 100u : 101u) << 8;
}

// ─── Escena: scanner ────────────────────────────────────────────────────────
// La marca, más un escáner de terminal de 8 bloques corriendo debajo. 54
// fotogramas de 40 ms (30 de 20 ms en busy): barrido hacia fuera, pausa,
// barrido de vuelta y luego un descanso largo que desvanece los bloques
// apagados del 60% al 18% de alfa.
#define SCAN_FRAMES      54
#define SCAN_FRAME_MS    40
#define SCAN_BUSY_FRAME_MS 20
#define SCAN_BUSY_FRAMES 30
#define SCAN_FADE_FROM   30          // los fotogramas 0..29 mantienen los apagados al 60%
#define SCAN_HEAD_FRAME  17          // antes de este fotograma la cabeza es el extremo derecho del rango
#define SCAN_LIMITED_FRAME 30        // en limited el escáner se congela aquí, con todos los bloques tenues
#define SCAN_BLINK_MS    500

// Rango de bloques encendidos [lo, hi], inclusive, por fotograma;
// 0xFF = nada encendido.
//   0-13  barrido a la derecha, 14-16 pausa, 17-29 barrido a la izquierda,
//   30-53 descanso, atenuándose (el SCANNER_LIT_RANGES del prototipo).
static const uint8_t SCAN_LIT[SCAN_FRAMES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x16, 0x27,   //  0- 7
    0x27, 0x37, 0x47, 0x57, 0x67, 0x77,               //  8-13
    0xFF, 0xFF, 0xFF,                               // 14-16
    0x67, 0x57, 0x47, 0x37, 0x27, 0x16, 0x05,        // 17-23
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00,               // 24-29
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // 30-37
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // 38-45
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // 46-53
};

// Alfa de la estela según la distancia a la cabeza: 0.65^(n-1) (el índice 0 es
// la cabeza y el 1 el bloque aclarado), redondeado a porcentaje.
static const uint8_t SCAN_TRAIL[8] = { 100, 90, 65, 42, 27, 18, 12, 8 };

#define SCAN_MARK_X  22
#define SCAN_MARK_Y  10
#define SCAN_MARK_SCALE 4
#define SCAN_STRIP_X 14
#define SCAN_STRIP_Y 38
#define SCAN_BLOCK   3
#define SCAN_GAP     1

// Alfa de los apagados: 60% durante el barrido, bajando al 18% durante el
// descanso (1.0 → 0.3 del fundido del prototipo, por su base de 0.6).
static int scan_unlit_pct(uint8_t frame) {
    if (frame < SCAN_FADE_FROM) return 60;
    const uint32_t d = (uint32_t)(frame - SCAN_FADE_FROM);
    return 60 - (int)((42 * d + 11) / 23);
}

static void scan_block(int x, uint8_t idx) {
    for (int r = 0; r < SCAN_BLOCK; r++) {
        const int dy = SCAN_STRIP_Y + r;
        for (int c = 0; c < SCAN_BLOCK; c++) {
            const int dx = SCAN_STRIP_X + x + c;
            if (dx >= 0 && dx < GRID && dy < GRID) cells[dy * GRID + dx] = idx;
        }
    }
}

static void scan_mark_cell(int mc, int mr, uint16_t color) {
    const uint8_t idx = pal_get(color);
    for (int sr = 0; sr < SCAN_MARK_SCALE; sr++) {
        const int dy = SCAN_MARK_Y + mr * SCAN_MARK_SCALE + sr;
        for (int sc = 0; sc < SCAN_MARK_SCALE; sc++) {
            const int dx = SCAN_MARK_X + mc * SCAN_MARK_SCALE + sc;
            if (dx >= 0 && dx < GRID && dy < GRID) cells[dy * GRID + dx] = idx;
        }
    }
}

static uint32_t compose_scanner(uint32_t elapsed, uint32_t key) {
    const bool limited = (mood == OC_MOOD_LIMITED);
    const bool busy    = (mood == OC_MOOD_BUSY);
    const uint8_t frame = limited ? SCAN_LIMITED_FRAME
                                  : (uint8_t)((elapsed /
                                       (busy ? SCAN_BUSY_FRAME_MS : SCAN_FRAME_MS)) %
                                      (busy ? SCAN_BUSY_FRAMES : SCAN_FRAMES));

    for (int mr = 0; mr < 5; mr++) {
        for (int mc = 0; mc < 4; mc++) {
            const uint16_t col = grid_color(OC_MARK[mr][mc]);
            if (col) scan_mark_cell(mc, mr, col);
        }
    }

    const uint8_t lit = SCAN_LIT[frame];
    const int lo = lit >> 4;
    const int hi = lit & 0x0F;
    const int head = (frame < SCAN_HEAD_FRAME) ? hi : lo;
    const int unlit = scan_unlit_pct(frame);
    for (int cell = 0; cell < 8; cell++) {
        uint32_t rgb = MOOD_BASE[mood];
        int alpha = unlit;
        if (lit != 0xFF && cell >= lo && cell <= hi) {
            const int trail = (cell > head) ? cell - head : head - cell;
            if (trail == 0) {
                rgb   = MOOD_HEAD[mood];
                alpha = 100;
            } else if (trail == 1) {
                rgb   = brighten(MOOD_BASE[mood]);
                alpha = 90;
            } else {
                alpha = SCAN_TRAIL[trail];
            }
        }
        scan_block(cell * (SCAN_BLOCK + SCAN_GAP), pal_get(shade(rgb, alpha)));
    }

    // En el límite el escáner se queda oscuro y el interior de la marca
    // parpadea en rojo.
    uint32_t blink = 0;
    if (limited) {
        blink = (elapsed / SCAN_BLINK_MS) % 2;
        const uint16_t col = blink ? hex565(COL_LIMITED) : hex565(COL_INNER);
        for (int i = 0; i < 4; i++) scan_mark_cell(MARK_INNER[i][0], MARK_INNER[i][1], col);
    }

    return key | ((uint32_t)frame | (blink << 8)) << 8;
}

// ─── Contenido de la paleta ─────────────────────────────────────────────────
// Pre-registra cada color que la escena actual puede dibujar, para que la
// correspondencia índice → color sea constante mientras corre y que la
// comparación por valores de la C6 no pueda perderse nunca un píxel cambiado.
// Presupuesto de ranuras: en el peor caso 30 de SPLASH_PALETTE_MAX — el escáner
// en limited: negro, dos colores de la marca, el rojo de limited, la cabeza, el
// escalón más brillante de la estela, los cuatro escalones finales (tres de los
// cuales coinciden con valores de la rampa) y la rampa de apagados de 24
// escalones. Typeon necesita 5, assemble 4.
static void pal_build(uint8_t s) {
    pal_reset();
    if (s == SCENE_TYPEON) {
        pal_get(hex565(COL_WORD));
        pal_get(hex565(COL_INNER));
        pal_get(hex565(COL_MARK));
        pal_get(shade(MOOD_BASE[mood], 100));            // el cursor de tecleo
    } else if (s == SCENE_ASSEMBLE) {
        pal_get(hex565(COL_MARK));                       // anillo
        pal_get(hex565(COL_INNER));                      // interior
        pal_get(hex565(COL_INNER_ALT));                  // interior, aclarado
    } else {
        pal_get(hex565(COL_MARK));                       // anillo de la marca
        pal_get(hex565(COL_INNER));                      // interior de la marca / parpadeo
        pal_get(hex565(COL_LIMITED));                    // parpadeo de limited
        pal_get(shade(MOOD_HEAD[mood], 100));            // cabeza del escáner
        pal_get(shade(brighten(MOOD_BASE[mood]), 90));    // escalón más brillante de la estela
        for (int t = 2; t <= 5; t++)                     // escalones finales, 0.65^(t-1)
            pal_get(shade(MOOD_BASE[mood], SCAN_TRAIL[t]));
        for (int f = SCAN_FADE_FROM; f < SCAN_FRAMES; f++)   // rampa de apagados
            pal_get(shade(MOOD_BASE[mood], scan_unlit_pct((uint8_t)f)));
    }
}

// ─── Composición ────────────────────────────────────────────────────────────
// Compone el escenario entero y devuelve una clave que identifica por completo
// el resultado: un tick cuya clave coincida con la última empujada pintaría un
// fotograma idéntico, así que se omite.
static uint32_t compose(uint32_t elapsed) {
    memset(cells, 0, sizeof(cells));
    uint32_t key = (uint32_t)scene | ((uint32_t)mood << 4);
    switch (scene) {
        case SCENE_TYPEON:   return compose_typeon(elapsed, key);
        case SCENE_ASSEMBLE: return compose_assemble(elapsed, key);
        default:             return compose_scanner(elapsed, key);
    }
}

static void scene_set(uint8_t s, uint32_t now) {
    scene    = s;
    scene_ms = now;
    next_ms  = now;      // componer el primer fotograma en el siguiente tick
    drawn    = false;
    pal_build(s);        // una escena nueva puede necesitar colores nuevos
}

// El instante en que el fotograma compuesto cambia por siguiente vez — el
// evento que mueve la escena: la siguiente letra que cae, el cursor que cambia
// de fase, la siguiente celda del anillo, el siguiente fotograma del escáner. Ms
// absolutos, derivados de las mismas constantes que usan los compositores. Esta
// es la puerta de retención de fotograma: el bucle principal va mucho más rápido
// que la animación, así que sin ella el escenario de 60×60 se recompondría en
// cada pasada.
static uint32_t next_change(uint32_t now) {
    const uint32_t e = now - scene_ms;
    switch (scene) {
        case SCENE_TYPEON: {
            if (e < TYPEON_LEAD_MS) return scene_ms + TYPEON_LEAD_MS;
            const uint32_t t = e - TYPEON_LEAD_MS;
            // El cursor cambia de fase cada TYPEON_CURSOR_MS...
            uint32_t at = TYPEON_LEAD_MS + (t / TYPEON_CURSOR_MS + 1) * TYPEON_CURSOR_MS;
            // ...y la letra k cae cuando se han sumado los retardos previos.
            uint32_t delay = 0;
            for (uint8_t i = 0; i < 8; i++) {
                if (delay > t) { if (TYPEON_LEAD_MS + delay < at) at = TYPEON_LEAD_MS + delay; break; }
                delay += TYPEON_DELAY[i];
            }
            return scene_ms + at;
        }
        case SCENE_ASSEMBLE: {
            const uint32_t intro = ASM_STEP_MS * 14;      // aparición del anillo
            if (e < intro) return scene_ms + (e / ASM_STEP_MS + 1) * ASM_STEP_MS;
            const uint32_t t = e - intro;                 // y luego el interior respira
            if (t < ASM_HOLD_MS) return scene_ms + intro + ASM_HOLD_MS;
            return scene_ms + intro + ASM_HOLD_MS +
                   ((t - ASM_HOLD_MS) / ASM_ALT_MS + 1) * ASM_ALT_MS;
        }
        default: {
            if (mood == OC_MOOD_LIMITED)                  // tira congelada; solo parpadeo
                return scene_ms + (e / SCAN_BLINK_MS + 1) * SCAN_BLINK_MS;
            const uint32_t step = (mood == OC_MOOD_BUSY) ? SCAN_BUSY_FRAME_MS : SCAN_FRAME_MS;
            return scene_ms + (e / step + 1) * step;
        }
    }
}

// ─── API pública ────────────────────────────────────────────────────────────
void oc_splash_start(void) {
    splash_set_external(true);
    active = true;
    manual = false;
    once   = false;
    scene_set(auto_scene(), millis());
}

void oc_splash_stop(void) {
    active = false;
    drawn  = false;
    splash_set_external(false);
}

void oc_splash_tick(void) {
    if (!active) return;
    const uint32_t now = millis();
    if (drawn && now < next_ms) return;
    if (once && now - scene_ms >= ASM_ONCE_MS) {
        once   = false;
        manual = false;
        scene_set(auto_scene(), now);
    }

    const uint32_t key = compose(now - scene_ms);
    next_ms = next_change(now);
    if (drawn && key == frame_key) return;   // no hay nada nuevo en pantalla
    frame_key = key;
    drawn     = true;
    splash_render_external(cells, pal);
}

void oc_splash_set_mood(oc_mood_t m) {
    if ((int)m < (int)OC_MOOD_IDLE || (int)m > (int)OC_MOOD_LIMITED) return;
    // Una carga útil llega más o menos una vez por minuto y suele repetir el
    // estado de ánimo; la escena no debe reiniciarse por un valor que ya está
    // mostrando.
    if (m == mood) return;
    mood = m;
    if (!active) return;                // no está en pantalla: oc_splash_start() lo elige

    if (!manual && auto_scene() != scene) {
        scene_set(auto_scene(), millis());       // la escena propia del estado es otra
    } else {
        // La misma escena, o una escena forzada que el prototipo también
        // conserva: la línea de tiempo sigue y solo cambia el tinte, así que el
        // wordmark no se vuelve a teclear y el escáner no salta al fotograma 0.
        // La paleta se reconstruye para el nuevo estado, y splash_render_external()
        // lo ve como una reasignación y responde con un repintado completo.
        pal_build(scene);
        drawn = false;
    }
}

void oc_splash_next_scene(void) {
    if (!active) return;
    manual = true;
    once   = false;
    scene_set((uint8_t)((scene + 1) % SCENE_COUNT), millis());
}

void oc_splash_play_assemble(void) {
    if (!active) return;
    manual = false;
    once   = true;
    scene_set(SCENE_ASSEMBLE, millis());
}

bool oc_splash_is_active(void) {
    return active;
}
