#include "splash.h"
#include "splash_animations.h"
#include "splash_geometry.h"
#include "theme.h"
#include "usage_rate.h"
#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// Escenario de 60×60. cell se dimensiona para que el lienzo quepa en la
// dimensión menor de la pantalla — el lienzo es cuadrado y está centrado, así
// que en paneles verticales o con bandas negras deja margen vertical en lugar
// de recortar. En placas sin PSRAM el búfer se renderiza diminuto (cell == 1)
// y LVGL lo escala para llenar el panel; la decisión de geometría vive en
// splash_compute_geometry() (splash_geometry.h).
//
// Las animaciones se guardan como recortes de su caja envolvente sobre el
// escenario de arte oficial de 55×37 (ver tools/convert_official_clawd.js);
// compose_stage() coloca el fotograma actual centrado en el escenario de 60×60.
// El escenario sobredimensionado deja sitio para después desplazar las
// animaciones por la pantalla (paseos, asomadas).
#define GRID         SPLASH_GRID
static int  cell      = 8;         // se recalcula en splash_init()
static int  canvas_w  = GRID * 8;
static int  canvas_h  = GRID * 8;

// Fondo del splash: negro puro (coincide con THEME_BG y con el índice 0 de
// paleta que emite tools/convert_official_clawd.js). Se usa para los márgenes
// del escenario y como paleta de reserva.
#define COL_EMPTY    0x0000

LV_FONT_DECLARE(font_styrene_28);

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // solo visible si no hay animaciones cargadas
static uint16_t *canvas_buf = NULL;        // 480x480 RGB565 (PSRAM)

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool active = false;

// Otro módulo es dueño del lienzo ahora mismo (oc_splash). El estado de Clawd
// se deja intacto — solo se bloquea el avance — así que después continúa desde
// la pose en la que se quedó.
static bool external = false;

// Mientras el splash está visible, pasar automáticamente a la siguiente
// animación del grupo actual (el que dicta el ritmo de consumo) cada estos ms.
#define SPLASH_ROTATE_INTERVAL_MS 20000

// Grupos de animación por ritmo de consumo: 4 grupos × hasta 4 animaciones
// cada uno. Se rellenan al iniciar emparejando los nombres literales de
// splash_anims[]. (jumping es la única animación sin asignar — sigue
// alcanzable mediante splash_next.)
#define GROUP_COUNT 4
#define GROUP_MAX   4
static int8_t  group_lists[GROUP_COUNT][GROUP_MAX];
static uint8_t group_size[GROUP_COUNT] = {0};
static uint8_t group_rotation[GROUP_COUNT] = {0};

static const char* GROUP_NAMES[GROUP_COUNT][GROUP_MAX] = {
    // Grupo 0 — reposo / adormilado (tranquilo, curioso). Magnifier primero:
    // es la elección de arranque, y empezar por lurking dejaría la pantalla
    // casi vacía al encender.
    { "magnifier", "walking", "pointing", "lurking" },
    // Grupo 1 — ritmo normal
    { "crab walking", "waving", "trumpet", "basketball" },
    // Grupo 2 — activo (tecleando contigo)
    { "laptop", "dancing", "skateboard", "soccer" },
    // Grupo 3 — consumo fuerte (escenas de mucha energía + el salto más
    // expansivo)
    { "racing car", "cloud", "sailing scene", "jumping happy" },
};

// Escenario de trabajo: el fotograma actual de la animación compuesto y
// centrado sobre la rejilla completa de 60×60 (índice 0 = fondo). 3,6 KB de RAM
// estática.
static uint8_t stage_cells[GRID * GRID];

// El escenario de arte oficial de 55×37 se ancla en una posición fija de la
// rejilla de 60×60, y cada animación se coloca en su desplazamiento original
// dentro del escenario (ox/oy) — nunca centrada una por una. Todas las
// animaciones comparten la misma posición de reposo de Clawd (x 15..38,
// y 21..36 en celdas del escenario), así que las transiciones entre ellas son
// continuas; centrar cada recorte haría saltar la pose de reposo.
#define STAGE_ANCHOR_X ((GRID - 55) / 2)
#define STAGE_ANCHOR_Y ((GRID - 37) / 2)

// ─── Reproducción: intro → bucle → outro ───────────────────────────────────
// Cada animación trae una región de bucle (ciclos de paso detectados por el
// conversor y tramos medios de escena; el archivo entero cuando nada se
// repite). La reproducción mantiene el bucle hasta que se libera — los
// caminantes lo liberan al llegar a su x destino, las escenas tras
// SCENE_LOOP_MS — y entonces suena el outro (recogida, salida del paso) y la
// animación termina en su pose de reposo. La rotación nunca corta en seco:
// libera el bucle y cambia después del outro, así que las transiciones ocurren
// siempre desde la pose de reposo compartida.
static bool     pb_done = false;        // terminada; mantiene el fotograma 0 de reposo
static bool     in_loop = false;
static bool     loop_release = false;
static uint32_t loop_entered_ms = 0;
static bool     pending_pick = false;   // rotación pedida; se aplica al terminar
#define SCENE_LOOP_MS 6000

// ─── Desplazamiento al caminar ──────────────────────────────────────────────
// Las marchas animan en el sitio; el recorrido por pantalla es nuestro,
// anclado a los pies: el movimiento por fotograma equivale a la deriva hacia
// atrás medida en los pies apoyados, así los pies apoyados no se mueven en
// pantalla.
//   crab walking (bucle de 8 fotogramas [1..8]): impulsos de 1 celda al entrar
//     en los fotogramas 4, 5, 8 y al cerrar el ciclo — 4 celdas / 640 ms
//     (6,25 celdas/s).
//   walking (bucle de 5 fotogramas [2..6]): 1,1,1,1,2 celdas → 6 celdas /
//     450 ms (~13,3 celdas/s).
// walk_begin(target) reproduce intro → bucle de marcha, se ajusta para caer
// exactamente en el destino y luego libera el bucle para que la marcha salga y
// Clawd se quede de pie. Al caminar hacia la izquierda el fotograma se espeja
// (los ojos van delante); la orientación se mantiene al estar de pie.
// DEMO: hasta que exista la máquina de estados por BLE, una coreografía repite
// de pie → borde derecho → fuera de pantalla por la izquierda → volver a casa.
enum WalkKind { WALK_NONE, WALK_CRAB, WALK_FRONT };
static WalkKind walk_kind = WALK_NONE;
static bool    walk_active = false;
static int     walk_x = 0;         // x del origen del fotograma en el escenario, puede ser < 0
static int     walk_dir = 0;       // -1 izquierda, +1 derecha, 0 de pie
static int     walk_target = 0;
static uint8_t walk_phase = 0;
static uint32_t walk_phase_started = 0;
static int     walk_home_x = 0;    // posición original a la que volver
static int     walk_face = +1;     // orientación, se conserva de pie (-1 = izquierda)

// Celdas que avanza el cuerpo cuando la marcha entra EN `frame` (ver cabecera).
static int walk_gait_cells_k(WalkKind kind, uint16_t frame, bool from_loop) {
    if (kind == WALK_CRAB) {
        if (frame == 1) return from_loop ? 1 : 0;     // cierre del ciclo, a impulso
        return (frame == 4 || frame == 5 || frame == 8) ? 1 : 0;
    }
    if (kind == WALK_FRONT) {
        if (frame < 2 || frame > 6) return 0;         // reposo / preparación / outro
        if (frame == 2 && !from_loop) return 0;       // primer apoyo
        return (frame == 6) ? 2 : 1;
    }
    return 0;
}
static int walk_gait_cells(uint16_t frame, bool from_loop) {
    return walk_gait_cells_k(walk_kind, frame, from_loop);
}

static void anim_reset(const splash_anim_def_t *a) {
    pb_done = false;
    in_loop = false;
    loop_release = false;
    pending_pick = false;
    walk_active = false;
    walk_kind = WALK_NONE;
    if (strcmp(a->name, "crab walking") == 0) walk_kind = WALK_CRAB;
    else if (strcmp(a->name, "walking") == 0) walk_kind = WALK_FRONT;
    else return;
    walk_active = true;
    walk_home_x = STAGE_ANCHOR_X + a->ox;
    walk_x = walk_home_x;
    walk_dir = 0;
    walk_face = +1;
    walk_phase = 0;
    walk_phase_started = millis();
    pb_done = true;    // los caminantes empiezan de pie; la coreografía arranla
}

static const uint8_t* compose_stage(const splash_anim_def_t *a, uint16_t frame);
static void render_frame(const uint8_t *cells, const uint16_t *palette);

// Empieza a caminar hacia target (x del origen del fotograma en el escenario).
static void walk_begin(int target) {
    if (target == walk_x) return;          // ya está ahí; se queda de pie
    walk_target = target;
    walk_dir = (target > walk_x) ? +1 : -1;
    walk_face = walk_dir;
    cur_frame = 0;
    frame_started_ms = millis();
    pb_done = false;
    loop_release = false;
    in_loop = false;
}

// Coreografía de demo: avanza de fase cada vez que termina la caminata actual.
static void walk_choreo(const splash_anim_def_t *a) {
    if (!pb_done) return;
    const uint32_t now = millis();
    switch (walk_phase) {
        case 0:  // de pie en casa
            if (now - walk_phase_started > 1200) { walk_phase = 1; walk_begin(GRID - a->w); }
            break;
        case 1:  // llegado al borde derecho
            walk_phase = 2; walk_phase_started = now;
            break;
        case 2:  // de pie en el borde
            if (now - walk_phase_started > 1200) { walk_phase = 3; walk_begin(-a->w); }
            break;
        case 3:  // fuera de pantalla por la izquierda
            walk_phase = 4; walk_phase_started = now;
            break;
        case 4:  // pausa fuera de pantalla (escenario vacío)
            if (now - walk_phase_started > 800) { walk_phase = 5; walk_begin(walk_home_x); }
            break;
        case 5:  // de vuelta en casa
            walk_phase = 0; walk_phase_started = now;
            break;
    }
}

static const uint8_t* compose_stage(const splash_anim_def_t *a, uint16_t frame) {
    memset(stage_cells, 0, sizeof(stage_cells));
    // Ajuste al borde horizontal: el arte que toca el borde izquierdo o derecho
    // de su lienzo está pensado para colgarse de ese borde (lurking asoma desde
    // la izquierda), así que va al borde real de la pantalla en lugar del borde
    // anclado del escenario. No se aplica en vertical — todas las animaciones
    // tocan el suelo del escenario, y la colocación vertical debe seguir
    // anclada (esquinas redondeadas del panel).
    int ax = STAGE_ANCHOR_X + a->ox;
    if (a->ox == 0)           ax = 0;
    if (a->ox + a->w == 55)   ax = GRID - a->w;
    if (walk_active)          ax = walk_x;
    const bool mirror = walk_active && walk_face < 0;
    const int ay = STAGE_ANCHOR_Y + a->oy;
    const uint8_t *src = &a->frames[(size_t)frame * a->w * a->h];
    for (int r = 0; r < a->h; r++) {
        const int dy = ay + r;
        if (dy < 0 || dy >= GRID) continue;
        int c0 = 0, c1 = a->w;                 // recorte para x parcialmente fuera de pantalla
        if (ax + c0 < 0)     c0 = -ax;
        if (ax + c1 > GRID)  c1 = GRID - ax;
        if (c0 >= c1) continue;
        if (mirror) {
            for (int c = c0; c < c1; c++)
                stage_cells[dy * GRID + ax + c] = src[r * a->w + (a->w - 1 - c)];
        } else {
            memcpy(&stage_cells[dy * GRID + ax + c0], &src[r * a->w + c0], c1 - c0);
        }
    }
    return stage_cells;
}

static void resolve_group_lists(void) {
    for (int g = 0; g < GROUP_COUNT; g++) {
        group_size[g] = 0;
        for (int s = 0; s < GROUP_MAX; s++) {
            group_lists[g][s] = -1;
            const char* want = GROUP_NAMES[g][s];
            if (!want) continue;
            for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
                if (strcmp(splash_anims[i].name, want) == 0) {
                    group_lists[g][group_size[g]++] = (int8_t)i;
                    break;
                }
            }
        }
    }
}

static uint16_t *row_buf = NULL;   // fila de trabajo, del tamaño de canvas_w (ruta PSRAM)

// ─── Dos rutas de renderizado ──────────────────────────────────────────────
// Las placas con PSRAM (S3) dibujan el pixel art en un lienzo LVGL a tamaño
// real y dejan que LVGL lo vuelque a la pantalla — tienen RAM y núcleos de
// sobra, no hace falta ninguna transformación.
//
// Las placas sin PSRAM (C6) no pueden albergar un lienzo de 480×480. El
// enfoque anterior (lienzo diminuto de 20×20 + image-scale de LVGL) obligaba a
// LVGL a transformar por software el fotograma completo escalado en cada
// redibujado — medido en ~0,76 µs/px de salida, es decir 100–220 ms por
// fotograma en el C6 de un solo núcleo, y la invalidación parcial de una imagen
// transformada ni recorta la transformación ni evita el arrastre. En su lugar
// escalamos nosotros las celdas del escenario con una replicación simple al
// vecino más próximo y enviamos solo las celdas *cambiadas* directamente al
// panel por el HAL de pantalla, sin pasar por LVGL. Eso elimina el coste de la
// transformación (queda solo el volcado QSPI) y el rectángulo sucio es exacto,
// así que no hay arrastre.
#ifndef BOARD_HAS_PSRAM
#  define SPLASH_DIRECT_DRAW 1
#else
#  define SPLASH_DIRECT_DRAW 0
#endif

#if SPLASH_DIRECT_DRAW
static uint16_t*       strip_buf = NULL;   // una banda de fila de la rejilla: (GRID*scr_cell)×scr_cell
static int             scr_cell  = 24;     // px en pantalla por celda de la rejilla
static int             scr_offx  = 0;      // desplazamientos de centrado (arte cuadrado en el panel)
static int             scr_offy  = 0;
static uint8_t         prev_cells[GRID * GRID];
static const uint16_t* prev_palette = NULL;
static uint16_t        prev_pal[SPLASH_PALETTE_MAX];   // última paleta vista, valor a valor
static bool            prev_valid   = false;
static bool            force_full   = false;  // repinta todo en el siguiente render
// Fotograma externo esperando el repintado diferido de más abajo (ver
// splash_render_external); los búferes del emisor siguen vivos entre ticks.
static const uint8_t*  ext_cells    = NULL;
static const uint16_t* ext_palette  = NULL;
static bool            ext_pending  = false;

// El rectángulo sucio de abajo compara los *valores* de las celdas, lo que solo
// tiene sentido mientras un índice conserva su color. Un dueño externo puede
// reescribir su paleta en el sitio entre fotogramas (el búfer es suyo), así que
// una paleta que cambia fuerza un repintado completo. 64 bytes de sombra,
// comparados en cada fotograma.
static bool palette_remapped(const uint16_t* palette) {
    if (!palette) return false;
    for (int i = 0; i < SPLASH_PALETTE_MAX; i++)
        if (prev_pal[i] != palette[i]) return true;
    return false;
}

// Escala las celdas de la rejilla [gx0..gx1]×[gy0..gy1] y las envía al panel,
// una banda de fila de cada vez, para que el búfer de trabajo siga siendo
// (GRID*scr_cell × scr_cell).
static void blit_cells(const uint8_t* cells, const uint16_t* palette,
                       int gx0, int gy0, int gx1, int gy1) {
    if (!strip_buf) return;
    const int spc = scr_cell;
    const int bw  = (gx1 - gx0 + 1) * spc;          // ancho de la banda, px
    const int px  = scr_offx + gx0 * spc;
    for (int gy = gy0; gy <= gy1; gy++) {
        for (int gx = gx0; gx <= gx1; gx++) {       // expande una fila de origen hacia los lados
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_MAX) ? palette[code] : COL_EMPTY;
            uint16_t* p = &strip_buf[(gx - gx0) * spc];
            for (int i = 0; i < spc; i++) p[i] = color;
        }
        for (int dy = 1; dy < spc; dy++)             // replica esa fila hacia abajo
            memcpy(&strip_buf[dy * bw], strip_buf, bw * 2);
        display_hal_draw_bitmap(px, scr_offy + gy * spc, bw, spc, strip_buf);
    }
}

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    if (!strip_buf) return;
    if (!active) return;          // nunca dibuja en el panel mientras no se muestra
    bool full = force_full || !prev_valid ||
                palette != prev_palette || palette_remapped(palette);
    force_full = false;

    int gx0 = 0, gy0 = 0, gx1 = GRID - 1, gy1 = GRID - 1;
    if (!full) {                                     // caja envolvente de las celdas que cambiaron
        gx0 = GRID; gy0 = GRID; gx1 = -1; gy1 = -1;
        for (int gy = 0; gy < GRID; gy++)
            for (int gx = 0; gx < GRID; gx++)
                if (cells[gy * GRID + gx] != prev_cells[gy * GRID + gx]) {
                    if (gx < gx0) gx0 = gx;
                    if (gx > gx1) gx1 = gx;
                    if (gy < gy0) gy0 = gy;
                    if (gy > gy1) gy1 = gy;
                }
        if (gx1 < 0) return;                         // fotograma idéntico, nada que hacer
    }

    blit_cells(cells, palette, gx0, gy0, gx1, gy1);

    memcpy(prev_cells, cells, GRID * GRID);
    prev_palette = palette;
    if (palette) memcpy(prev_pal, palette, sizeof(prev_pal));
    prev_valid   = true;
}

#else  // ── PSRAM: render en lienzo LVGL (sin cambios) ──

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    if (!row_buf || !canvas_buf) return;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_MAX) ? palette[code] : COL_EMPTY;
            uint16_t *p = &row_buf[gx * cell];
            for (int i = 0; i < cell; i++) p[i] = color;
        }
        for (int dy = 0; dy < cell; dy++) {
            memcpy(&canvas_buf[(gy * cell + dy) * canvas_w], row_buf, canvas_w * 2);
        }
    }
    if (canvas) lv_obj_invalidate(canvas);
}
#endif

// ---- Criatura mini: una criatura animada pequeña para incrustar en otras
//      pantallas (p. ej. el indicador de reposo). Es autónoma — su propio lienzo
//      y su propio búfer, independiente del splash de pantalla completa de
//      arriba. ----
static lv_obj_t  *mini_canvas = NULL;
static uint16_t  *mini_buf = NULL;
static int        mini_cell = 0;
static int        mini_w = 0;      // px del lienzo, mini_anim->w * mini_cell
static int        mini_h = 0;
static const splash_anim_def_t *mini_anim = NULL;
static uint16_t   mini_frame = 0;
static uint32_t   mini_started = 0;

static void mini_render(void) {
    if (!mini_buf || !mini_anim) return;
    const int aw = mini_anim->w, ah = mini_anim->h;
    const uint8_t *cells = &mini_anim->frames[(size_t)mini_frame * aw * ah];
    const uint16_t *pal = mini_anim->palette;
    for (int gy = 0; gy < ah; gy++) {
        for (int gx = 0; gx < aw; gx++) {
            uint8_t code = cells[gy * aw + gx];
            uint16_t color = (pal && code < SPLASH_PALETTE_SIZE) ? pal[code] : COL_EMPTY;
            for (int dy = 0; dy < mini_cell; dy++) {
                uint16_t *dst = &mini_buf[(gy * mini_cell + dy) * mini_w + gx * mini_cell];
                for (int dx = 0; dx < mini_cell; dx++) dst[dx] = color;
            }
        }
    }
    if (mini_canvas) lv_obj_invalidate(mini_canvas);
}

lv_obj_t* splash_mini_create(lv_obj_t *parent, const char *anim_name, int px) {
    mini_anim = NULL;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, anim_name) == 0) { mini_anim = &splash_anims[i]; break; }
    }
    if (!mini_anim) return NULL;
    const int amax = (mini_anim->w > mini_anim->h) ? mini_anim->w : mini_anim->h;
    mini_cell = px / amax;
    if (mini_cell < 1) mini_cell = 1;
    mini_w = mini_anim->w * mini_cell;
    mini_h = mini_anim->h * mini_cell;
#ifdef BOARD_HAS_PSRAM
    const uint32_t caps = MALLOC_CAP_SPIRAM;
#else
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    mini_buf = (uint16_t*)heap_caps_malloc(mini_w * mini_h * 2, caps);
    if (!mini_buf) return NULL;
    mini_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(mini_canvas, mini_buf, mini_w, mini_h, LV_COLOR_FORMAT_RGB565);
    mini_frame = 0;
    mini_started = millis();
    mini_render();
    return mini_canvas;
}

void splash_mini_tick(void) {
    if (!mini_buf || !mini_anim || mini_anim->frame_count == 0) return;
    if (millis() - mini_started < mini_anim->holds[mini_frame]) return;
    mini_started = millis();
    mini_frame = (mini_frame + 1) % mini_anim->frame_count;
    mini_render();
}

// ─── Mascota de esquina (pantalla de consumo) ───────────────────────────────
// El hueco del logo de la esquina, con vida: el Clawd quieto espera, de vez en
// cuando hace un gesto pequeño (saludar, bailar, señalar) en su sitio, y cada
// unos cuantos gestos se sale por el borde izquierdo, hace la animación lurking
// a tamaño completo sobre la pantalla y vuelve andando a su hueco. Solo en
// placas con PSRAM (ui.cpp usa el icono estático clawd_still.h en la C6); la
// mueve splash_mascot_tick() desde el bucle principal, con independencia del
// propio splash.
static lv_obj_t *mas_img = NULL;
static lv_obj_t *mas_lurk_img = NULL;
static uint8_t  *mas_buf = NULL;       // RGB565A8 planar, del tamaño del gesto más grande
static uint8_t  *mas_lurk_buf = NULL;
static lv_image_dsc_t mas_dsc, mas_lurk_dsc;
static int  mas_cell = 3;
static int  mas_slot_x = 0;            // px del hueco (destino de la vuelta andando)
static int  mas_feet_y = 0;            // px de la línea de pies (todo el arte se apoya en el suelo)
static int  mas_lurk_cell = 8;
static int  mas_screen_w = 480;
static bool mas_visible = false;

enum MasMode { MAS_STILL, MAS_ACT, MAS_WALK_OFF, MAS_LURK, MAS_WALK_IN };
static MasMode mas_mode = MAS_STILL;
static const splash_anim_def_t *mas_anim = NULL;
static uint16_t mas_frame = 0;
static uint32_t mas_frame_started = 0;
static uint32_t mas_mode_started = 0;
static int  mas_x = 0;                 // x del widget, px (puede quedar fuera de pantalla)
static int  mas_face = +1;
static uint8_t mas_act_idx = 0;
static bool mas_from_loop = false;

// La mascota de esquina refleja el ánimo del splash: por cada grupo de ritmo de
// consumo, cuánto espera entre gestos y qué gestos hace. lurking es el viaje de
// salir andando / asomar a tamaño completo / volver andando. Los gestos tienen
// que caber en el búfer de 28×21 celdas (los saltos son demasiado altos para la
// esquina).
static const char* MAS_ACTS_BY_RATE[4][4] = {
    { "pointing", "lurking", NULL,       NULL      },   // reposo: espaciado y furtivo
    { "waving",   "lurking", "pointing", NULL      },   // normal
    { "waving",   "dancing", "lurking",  NULL      },   // activo
    { "dancing",  "waving",  "dancing",  "lurking" },   // consumo fuerte: no puede estarse quieto
};
static const uint16_t MAS_STILL_MS_BY_RATE[4] = { 10000, 7000, 5000, 3500 };

static const splash_anim_def_t* anim_by_name(const char *n) {
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++)
        if (strcmp(splash_anims[i].name, n) == 0) return &splash_anims[i];
    return NULL;
}

// Renderiza un fotograma en una imagen planar RGB565A8 (alfa 0 fuera del arte) y
// ancla el widget en la línea de pies compartida.
static void mas_render(const splash_anim_def_t *a, uint16_t frame, bool mirror,
                       lv_image_dsc_t *dsc, uint8_t *buf, lv_obj_t *img,
                       int cell, int x, int feet_y) {
    const int w = a->w * cell, h = a->h * cell;
    uint16_t *color = (uint16_t*)buf;
    uint8_t  *alpha = buf + (size_t)w * h * 2;
    const uint8_t *src = &a->frames[(size_t)frame * a->w * a->h];
    for (int gy = 0; gy < a->h; gy++) {
        for (int gx = 0; gx < a->w; gx++) {
            uint8_t code = src[gy * a->w + (mirror ? a->w - 1 - gx : gx)];
            uint16_t c = (code && code < SPLASH_PALETTE_SIZE) ? a->palette[code] : 0;
            uint8_t  al = code ? 255 : 0;
            for (int dy = 0; dy < cell; dy++) {
                uint16_t *cp = &color[(gy * cell + dy) * w + gx * cell];
                uint8_t  *ap = &alpha[(gy * cell + dy) * w + gx * cell];
                for (int dx = 0; dx < cell; dx++) { cp[dx] = c; ap[dx] = al; }
            }
        }
    }
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = buf;
    dsc->data_size = (size_t)w * h * 3;
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, x, feet_y - h);
    lv_obj_invalidate(img);
}

static void mas_show_still(void) {
    mas_anim = anim_by_name("walking");     // el fotograma 0 es la pose oficial de reposo
    mas_frame = 0;
    mas_mode = MAS_STILL;
    mas_mode_started = millis();
    mas_x = mas_slot_x;
    mas_face = +1;
    if (mas_anim)
        mas_render(mas_anim, 0, false, &mas_dsc, mas_buf, mas_img,
                   mas_cell, mas_x, mas_feet_y);
}

lv_obj_t* splash_mascot_create(lv_obj_t *parent, int slot_x, int feet_y, int cell) {
    mas_cell = cell;
    mas_slot_x = slot_x;
    mas_feet_y = feet_y;
    mas_screen_w = board_caps().width;
    // Búfer para la caja envolvente del gesto más grande (pointing, 28×21 celdas).
    const size_t mas_bytes = (size_t)(28 * cell) * (21 * cell) * 3;
    const splash_anim_def_t *lurk = anim_by_name("lurking");
    const BoardCaps& c = board_caps();
    int mind = (c.width < c.height) ? c.width : c.height;
    mas_lurk_cell = mind / SPLASH_GRID;
    if (mas_lurk_cell < 1) mas_lurk_cell = 1;
    const size_t lurk_bytes = lurk ?
        (size_t)(lurk->w * mas_lurk_cell) * (lurk->h * mas_lurk_cell) * 3 : 0;
    mas_buf      = (uint8_t*)heap_caps_malloc(mas_bytes,  MALLOC_CAP_SPIRAM);
    mas_lurk_buf = lurk_bytes ? (uint8_t*)heap_caps_malloc(lurk_bytes, MALLOC_CAP_SPIRAM) : NULL;
    if (!mas_buf) return NULL;
    mas_img = lv_image_create(parent);
    if (mas_lurk_buf) {
        mas_lurk_img = lv_image_create(parent);
        lv_obj_add_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);
    }
    mas_show_still();
    return mas_img;
}

void splash_mascot_set_visible(bool v) {
    mas_visible = v;
    if (!mas_img) return;
    if (v) {
        lv_obj_clear_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
        // La mascota pasa por encima de todo — déjala por delante de sus
        // hermanas creadas después (icono de batería, etiquetas) siempre que se
        // muestre.
        lv_obj_move_foreground(mas_img);
        if (mas_lurk_img) lv_obj_move_foreground(mas_lurk_img);
        mas_show_still();                       // reinicio limpio en su hueco
    } else {
        lv_obj_add_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
        if (mas_lurk_img) lv_obj_add_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void splash_mascot_tick(void) {
    // Mientras un dueño externo tiene el lienzo del splash, el hueco de la
    // esquina queda fuera del splash de Clawd — mejor quedarse quieta que
    // animar por encima.
    if (external) return;
    if (!mas_img || !mas_visible || !mas_anim) return;
    const uint32_t now = millis();

    if (mas_mode == MAS_STILL) {
        int g = usage_rate_group();
        if (g < 0 || g > 3) g = 0;
        if (now - mas_mode_started < MAS_STILL_MS_BY_RATE[g]) return;
        uint8_t count = 0;
        while (count < 4 && MAS_ACTS_BY_RATE[g][count]) count++;
        if (count == 0) { mas_mode_started = now; return; }
        const char *act = MAS_ACTS_BY_RATE[g][mas_act_idx++ % count];
        mas_frame = 0;
        mas_frame_started = now;
        mas_from_loop = false;
        if (strcmp(act, "lurking") == 0 && mas_lurk_img) {   // el viaje de asomarse
            mas_anim = anim_by_name("walking");
            mas_face = -1;
            mas_mode = MAS_WALK_OFF;
        } else {
            const splash_anim_def_t *a = anim_by_name(act);
            if (!a) { mas_mode_started = now; return; }
            mas_anim = a;
            mas_face = +1;
            mas_mode = MAS_ACT;
        }
        return;
    }

    const splash_anim_def_t *a = mas_anim;
    if (now - mas_frame_started < a->holds[mas_frame]) return;
    mas_frame_started = now;

    uint16_t next = mas_frame + 1;
    const bool walking_mode = (mas_mode == MAS_WALK_OFF || mas_mode == MAS_WALK_IN);
    if (walking_mode && mas_frame == a->loop_end)
        next = a->loop_start;                       // al caminar: mantén el bucle de la marcha

    if (next >= a->frame_count) {                   // gesto / asomada terminada
        if (mas_mode == MAS_LURK) {
            lv_obj_add_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);
            mas_anim = anim_by_name("walking");
            mas_frame = 0;
            mas_from_loop = false;
            mas_face = -1;                          // se asomó por la derecha,
            mas_x = mas_screen_w;                   // así que vuelve desde ahí
            mas_mode = MAS_WALK_IN;
            lv_obj_clear_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        mas_show_still();                           // los gestos acaban en la pose de reposo
        return;
    }

    const bool from_loop = mas_from_loop;
    mas_frame = next;
    mas_from_loop = walking_mode &&
        mas_frame >= a->loop_start && mas_frame <= a->loop_end;

    if (walking_mode && mas_from_loop) {
        const int step = walk_gait_cells_k(WALK_FRONT, mas_frame, from_loop) * mas_cell;
        // La salida siempre se va por la izquierda; la vuelta se dirige al hueco
        // desde el lado en el que esté (la derecha, tras la asomada).
        const int dir = (mas_mode == MAS_WALK_OFF) ? -1
                        : (mas_x < mas_slot_x ? +1 : -1);
        mas_face = (mas_mode == MAS_WALK_OFF) ? -1 : dir;
        mas_x += dir * step;
        if (mas_mode == MAS_WALK_OFF && mas_x <= -a->w * mas_cell) {
            // Fuera del todo: oculta el sprite de la esquina y lanza la asomada a
            // tamaño completo.
            lv_obj_add_flag(mas_img, LV_OBJ_FLAG_HIDDEN);
            const splash_anim_def_t *lurk = anim_by_name("lurking");
            if (lurk && mas_lurk_img && mas_lurk_buf) {
                mas_anim = lurk;
                mas_frame = 0;
                mas_mode = MAS_LURK;
                lv_obj_clear_flag(mas_lurk_img, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(mas_lurk_img);
                // Salió por la izquierda, así que asoma por el borde DERECHO —
                // espejado al renderizar (el arte está dibujado para el borde
                // izquierdo).
                mas_render(lurk, 0, true, &mas_lurk_dsc, mas_lurk_buf,
                           mas_lurk_img, mas_lurk_cell,
                           mas_screen_w - lurk->w * mas_lurk_cell,
                           (STAGE_ANCHOR_Y + lurk->oy + lurk->h) * mas_lurk_cell);
            } else {
                mas_mode = MAS_WALK_IN;             // no hay material de lurking: dar la vuelta
                mas_face = +1;
            }
            return;
        }
        if (mas_mode == MAS_WALK_IN &&
            ((dir > 0 && mas_x >= mas_slot_x) || (dir < 0 && mas_x <= mas_slot_x))) {
            mas_show_still();                       // llegó: se acomoda en su hueco
            return;
        }
    }

    if (mas_mode == MAS_LURK) {
        mas_render(a, mas_frame, true, &mas_lurk_dsc, mas_lurk_buf, mas_lurk_img,
                   mas_lurk_cell, mas_screen_w - a->w * mas_lurk_cell,
                   (STAGE_ANCHOR_Y + a->oy + a->h) * mas_lurk_cell);
    } else {
        mas_render(a, mas_frame, mas_face < 0, &mas_dsc, mas_buf, mas_img,
                   mas_cell, mas_x, mas_feet_y);
    }
}

static void show_placeholder() {
    // Fondo oscuro sólido + etiqueta de estado centrada. En la ruta de dibujo
    // directo no hay lienzo; el contenedor negro es el fondo y la etiqueta LVGL
    // se dibuja encima.
#if !SPLASH_DIRECT_DRAW
    if (canvas_buf) {
        for (int i = 0; i < canvas_w * canvas_h; i++) canvas_buf[i] = COL_EMPTY;
    }
    if (canvas) lv_obj_invalidate(canvas);
#endif
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    const BoardCaps& c = board_caps();

    // Contenedor negro compartido a pantalla completa — el fondo del splash.
    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, c.width, c.height);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

#if SPLASH_DIRECT_DRAW
    // Ruta directa al panel (sin PSRAM): sin lienzo LVGL. Calcula el tamaño de
    // celda en pantalla + el centrado, y un búfer de banda de trabajo del ancho
    // de una tira de fila de la rejilla sobre el arte cuadrado
    // (GRID*scr_cell × scr_cell). En la C6 son 480×24×2 ≈ 23 KB de SRAM interna.
    int mind = (c.width < c.height) ? c.width : c.height;
    scr_cell = mind / GRID;
    int side = GRID * scr_cell;
    scr_offx = (c.width  - side) / 2;
    scr_offy = (c.height - side) / 2;
    strip_buf = (uint16_t*)heap_caps_malloc((size_t)side * scr_cell * 2,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!strip_buf) {
        Serial.println("splash: strip buffer alloc failed");
        return;
    }
#else
    // Ruta PSRAM: renderiza en un lienzo LVGL a tamaño real (sin transformación).
    SplashGeometry geo = splash_compute_geometry(c.width, c.height, true);
    cell                = geo.cell;
    canvas_w            = geo.canvas_dim;
    canvas_h            = geo.canvas_dim;
    const int img_scale = geo.scale;

    canvas_buf = (uint16_t*)heap_caps_malloc(canvas_w * canvas_h * 2, MALLOC_CAP_SPIRAM);
    row_buf    = (uint16_t*)heap_caps_malloc(canvas_w * 2,            MALLOC_CAP_SPIRAM);
    if (!canvas_buf || !row_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
    if (img_scale != SPLASH_SCALE_UNITY) {
        lv_image_set_antialias(canvas, false);
        lv_image_set_pivot(canvas, canvas_w / 2, canvas_h / 2);
        lv_image_set_scale(canvas, img_scale);
    }
    lv_obj_center(canvas);
#endif

    // Etiqueta de marcador de posición (visible solo si no hay animaciones
    // cargadas)
    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "sin animaciones cargadas\n\n"
        "ejecuta tools/convert_official_clawd.js");
    lv_obj_set_style_text_font(label_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();

    if (SPLASH_ANIM_COUNT == 0) {
        show_placeholder();
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
#if !SPLASH_DIRECT_DRAW
        // La ruta PSRAM prerrenderiza el fotograma 0 en el búfer del lienzo. La
        // ruta directa no dibuja nada aquí — render_frame() se sale mientras está
        // inactiva, así que el splash nunca pinta en el panel antes de mostrarse
        // de verdad.
        const splash_anim_def_t *a = &splash_anims[0];
        render_frame(compose_stage(a, 0), a->palette);
#endif
        frame_started_ms = millis();
    }

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

void splash_tick(void) {
#if SPLASH_DIRECT_DRAW
    // Repintado completo diferido tras (volver a) mostrar — se ejecuta ahora
    // que LVGL ha dibujado el fondo negro en esta iteración del bucle. Un dueño
    // externo entrega aquí su propio fotograma; si no, Clawd repinta su pose
    // actual.
    if (force_full || ext_pending) {
        if (external) {
            if (ext_pending) render_frame(ext_cells, ext_palette);
        } else if (SPLASH_ANIM_COUNT) {
            const splash_anim_def_t *fa = &splash_anims[cur_anim];
            if (fa->frame_count) render_frame(compose_stage(fa, cur_frame), fa->palette);
        }
        ext_pending = false;
    }
#endif

    if (external) return;          // el lienzo es de otro; Clawd se congela
    if (!active || SPLASH_ANIM_COUNT == 0) return;
    const uint32_t now = millis();

    const splash_anim_def_t *a = &splash_anims[cur_anim];
    if (a->frame_count == 0) return;

    if (walk_active) walk_choreo(a);

    // Escenas: mantiene el bucle SCENE_LOOP_MS y luego deja sonar el outro.
    if (!walk_active && in_loop && !loop_release &&
        now - loop_entered_ms >= SCENE_LOOP_MS)
        loop_release = true;

    // Rotación automática — nunca un corte seco. Los caminantes cambian solo
    // estando de pie en casa; todo lo demás libera su bucle y cambia después del
    // outro.
    if (now - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        if (walk_active) {
            if (walk_phase == 0 && pb_done) splash_pick_for_current_rate();
        } else {
            loop_release = true;
            pending_pick = true;
            last_pick_ms = now;    // no vuelvas a disparar mientras suena el outro
        }
    }

    if (pb_done) return;                       // mantiene el fotograma de reposo
    if (now - frame_started_ms < a->holds[cur_frame]) return;

    // Avanza un fotograma por intro → bucle → outro.
    const bool from_loop = in_loop;
    uint16_t next = cur_frame + 1;
    if (cur_frame == a->loop_end && !loop_release)
        next = a->loop_start;

    if (next >= a->frame_count) {              // se completó el archivo
        if (pending_pick) {
            pending_pick = false;
            splash_pick_for_current_rate();
            return;
        }
        if (walk_active) {                     // caminata terminada: quedarse de pie
            cur_frame = 0;
            frame_started_ms = now;
            pb_done = true;
            render_frame(compose_stage(a, 0), a->palette);
            return;
        }
        next = 0;                              // repetir desde la intro
        loop_release = false;
    }

    cur_frame = next;
    frame_started_ms = now;
    const bool now_in = cur_frame >= a->loop_start && cur_frame <= a->loop_end;
    if (now_in && !from_loop) loop_entered_ms = now;
    in_loop = now_in;

    // Desplazamiento al caminar, anclado a los fotogramas de marcha; se ajusta
    // para caer exactamente en el destino y luego libera el bucle para que la
    // marcha salga.
    if (walk_active && walk_dir != 0 && in_loop) {
        walk_x += walk_dir * walk_gait_cells(cur_frame, from_loop);
        if ((walk_dir > 0 && walk_x >= walk_target) ||
            (walk_dir < 0 && walk_x <= walk_target)) {
            walk_x = walk_target;
            walk_dir = 0;
            loop_release = true;
        }
    }

    render_frame(compose_stage(a, cur_frame), a->palette);
}

void splash_next(void) {
    if (external || SPLASH_ANIM_COUNT == 0) return;
    cur_anim = (cur_anim + 1) % SPLASH_ANIM_COUNT;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    anim_reset(a);
    render_frame(compose_stage(a, 0), a->palette);
    Serial.printf("splash: -> %s\n", a->name);
}

void splash_pick_for_current_rate(void) {
    if (external || SPLASH_ANIM_COUNT == 0) return;
    int g = usage_rate_group();
    if (g < 0 || g >= GROUP_COUNT) g = 0;
    if (group_size[g] == 0) return;

    uint8_t slot = group_rotation[g] % group_size[g];
    group_rotation[g]++;
    int8_t idx = group_lists[g][slot];
    if (idx < 0) return;

    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    anim_reset(a);
    render_frame(compose_stage(a, 0), a->palette);
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    if (!external) splash_pick_for_current_rate();  // la ruta directa difiere el dibujo
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
#if SPLASH_DIRECT_DRAW
    // LVGL rellena el contenedor de negro una sola vez al mostrarlo; eso borraría
    // una criatura dibujada ahora. Difiere el repintado completo al siguiente
    // splash_tick(), que se ejecuta después de lv_timer_handler() en el bucle
    // principal.
    force_full = true;
#endif
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}

void splash_set_external(bool on) {
    if (external == on) return;
    external = on;
    if (external) {
#if SPLASH_DIRECT_DRAW
        // El primer fotograma del dueño es un repintado completo, que además
        // borra el arte de Clawd y los márgenes que dejaba.
        force_full  = true;
        prev_valid  = false;
        ext_cells   = NULL;
        ext_palette = NULL;
        ext_pending = false;
#endif
        return;
    }
    // Devolver el lienzo: repinta a Clawd desde cero y reinicia sus relojes, para
    // que una toma externa larga no parezca una ráfaga de fotogramas o una
    // rotación de ritmo en el momento en que vuelve el splash.
    frame_started_ms = millis();
    last_pick_ms     = frame_started_ms;
#if SPLASH_DIRECT_DRAW
    force_full  = true;
    prev_valid  = false;
    ext_cells   = NULL;
    ext_palette = NULL;
    ext_pending = false;
#endif
}

void splash_render_external(const uint8_t *cells, const uint16_t *palette) {
    if (!cells) return;
#if SPLASH_DIRECT_DRAW
    // LVGL repinta el fondo del contenedor cuando se muestra, lo que borraría un
    // fotograma dibujado en la misma pasada del bucle. Mientras ese repintado
    // siga pendiente, guarda el fotograma para el siguiente splash_tick()
    // (después de lv_timer_handler()).
    if (force_full) {
        ext_cells   = cells;
        ext_palette = palette;
        ext_pending = true;
        return;
    }
#endif
    render_frame(cells, palette);
}
