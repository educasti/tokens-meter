#include "ui_sys.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "hal/board_caps.h"
#include "hal/power_hal.h"
#include "ble.h"
#include "portal.h"
#include "ota.h"
#include "ota_pull.h"
#include "brightness.h"

// Command Center (design/command-center/SPEC.md). Overlay topmost con dos
// tarjetas, mismo lenguaje que el portafolio: fondo negro, tarjetas #1a1a1a
// radio 12, Plex Mono (los cortes 48/18/16/12 ya traen los glifos ES mas
// U+00B7 y U+2026, asi que las cadenas de la seccion 8 se escriben tal cual;
// · y … van con escapes \xC2\xB7 / \xE2\x80\xA6 como en ui_opencode).
// Desvio anotado: la SPEC pide hint en Plex 14 pero ese corte no existe en
// firmware (solo 48/18/16/12 con glifos ES); el hint usa Plex 16 en grande y
// Plex 12 en compacto/chico.
//
// Lectura en vivo (SPEC seccion 6): sys_tick()/sys_pwr_action() leen SIEMPRE
// primero todas las APIs y formatean a buffers propios; paint() solo toca
// LVGL con esos buffers ya leidos. LVGL corre unicamente en la tarea del
// loop (sys_tick va enganchado en ui_tick_anim()) y el snapshot pull llega
// copiado bajo mux desde ota_pull_ui_snapshot().

// Paleta del portafolio (ui_portfolio.cpp): superficies oscuras, texto claro,
// muted para etiquetas, verde/rojo solo para estados, melocoton de aviso.
#define SYS_SURFACE   lv_color_hex(0x1a1a1a)
#define SYS_TEXT      lv_color_hex(0xeeeeee)
#define SYS_MUTED     lv_color_hex(0x8c8c8c)
#define SYS_SUCCESS   lv_color_hex(0x2ee88a)
#define SYS_ERROR     lv_color_hex(0xe06c75)
#define SYS_AMBER     lv_color_hex(0xfab283)
#define SYS_TRACK     lv_color_hex(0x333333)   // riel de la barra de descarga

LV_FONT_DECLARE(font_plex_48);
LV_FONT_DECLARE(font_plex_18);
LV_FONT_DECLARE(font_plex_16);
LV_FONT_DECLARE(font_plex_12);

#define SYS_MIDDOT    "\xC2\xB7"              // ·
#define SYS_ELLIPSIS  "\xE2\x80\xA6"          // …

// Confirmacion de aplicado por doble PWR (SPEC seccion 5): el primer PWR arma,
// el segundo dentro de la ventana aplica. sys_tick desarma al vencer.
#define SYS_ARM_MS    3500u
// SSID de hasta 63: se trunca a ~18 + '.' (patron fmt_sym del portafolio,
// exigido por la SPEC seccion 9 para check_label_widths.py).
#define SYS_SSID_MAX  18

// ---- Geometria ------------------------------------------------------------
// Los mismos 3 breakpoints que ui_portfolio.cpp: grande = 480x480 (corte de
// la SPEC seccion 4, tal cual), compacto y chico a criterio con la misma
// estructura. card_w sale siempre de scr_w - 2 * card_x.
struct SysLayout {
    int16_t scr_w, scr_h;
    int16_t header_x, header_top;
    const lv_font_t *header_font;
    int16_t card_x, card_w, pad;
    int16_t c1_y, c1_h, c1_radius;
    int16_t c2_y, c2_h, c2_radius;
    bool     has_label;                       // el chico no tiene etiqueta en card 1
    int16_t  label_top;  const lv_font_t *label_font;
    int16_t  board_top, board_ri;  const lv_font_t *board_font;
    int16_t  hero_top;   const lv_font_t *hero_font;
    int16_t  ota_top;    const lv_font_t *ota_font;
    int16_t  act_top;    const lv_font_t *act_font;
    int16_t  bar_y, bar_h, sub_top;  const lv_font_t *sub_font;
    int16_t  title_top;  const lv_font_t *title_font;
    int16_t  rows_top, rows_pitch;   const lv_font_t *rows_font;
    int16_t  hint_top;   const lv_font_t *hint_font;
    int16_t  st_top;     const lv_font_t *st_font;
};
static SysLayout L = {};

static void compute_layout(const BoardCaps &c) {
    L.scr_w = c.width;
    L.scr_h = c.height;

    if (c.width >= 460 && c.height >= 460) {
        // ---- grande: 480x480 (SPEC seccion 4) ----
        L.header_x = 20; L.header_top = 36;  L.header_font = &font_plex_16;
        L.card_x = 20;  L.pad = 20;
        L.c1_y = 72;  L.c1_h = 162; L.c1_radius = 12;      // 72..234
        L.c2_y = 246; L.c2_h = 182; L.c2_radius = 12;      // 246..428 (4 filas)
        L.has_label = true;
        L.label_top = 14; L.label_font = &font_plex_16;
        L.board_top = 16; L.board_ri = 20; L.board_font = &font_plex_12;
        L.hero_top = 34;  L.hero_font = &font_plex_48;
        L.ota_top = 98;   L.ota_font = &font_plex_18;
        L.act_top = 128;  L.act_font = &font_plex_16;
        L.bar_y = 128; L.bar_h = 12; L.sub_top = 142; L.sub_font = &font_plex_12;
        L.title_top = 14; L.title_font = &font_plex_16;
        L.rows_top = 46; L.rows_pitch = 30;
        L.rows_font = &font_plex_16;
        L.hint_top = 436; L.hint_font = &font_plex_16;
        L.st_top = 458;   L.st_font = &font_plex_16;
    } else if (c.height >= 300) {
        // ---- compacto: 368x448 ----
        L.header_x = 20; L.header_top = 28;  L.header_font = &font_plex_16;
        L.card_x = 20;  L.pad = 16;
        L.c1_y = 60;  L.c1_h = 150; L.c1_radius = 12;
        L.c2_y = 218; L.c2_h = 164; L.c2_radius = 12;      // 218..382 (4 filas)
        L.has_label = true;
        L.label_top = 12; L.label_font = &font_plex_16;
        L.board_top = 14; L.board_ri = 16; L.board_font = &font_plex_12;
        L.hero_top = 30;  L.hero_font = &font_plex_48;
        L.ota_top = 90;   L.ota_font = &font_plex_18;
        L.act_top = 120;  L.act_font = &font_plex_16;
        L.bar_y = 120; L.bar_h = 10; L.sub_top = 134; L.sub_font = &font_plex_12;
        L.title_top = 12; L.title_font = &font_plex_16;
        L.rows_top = 40; L.rows_pitch = 28;
        L.rows_font = &font_plex_16;
        L.hint_top = 390; L.hint_font = &font_plex_12;
        L.st_top = 412;   L.st_font = &font_plex_16;
    } else {
        // ---- chico: 240x240 ----
        L.header_x = 8; L.header_top = 8;  L.header_font = &font_plex_12;
        L.card_x = 8;  L.pad = 8;
        L.c1_y = 24;  L.c1_h = 92; L.c1_radius = 8;
        L.c2_y = 122; L.c2_h = 90; L.c2_radius = 8;        // 122..212 (4 filas)
        L.has_label = false;           // sin sitio: el heroe es la etiqueta
        L.label_top = 0; L.label_font = &font_plex_12;
        L.board_top = 6; L.board_ri = 8; L.board_font = &font_plex_12;
        L.hero_top = 16;  L.hero_font = &font_plex_18;
        L.ota_top = 42;   L.ota_font = &font_plex_12;
        L.act_top = 64;   L.act_font = &font_plex_12;
        L.bar_y = 64; L.bar_h = 8; L.sub_top = 76; L.sub_font = &font_plex_12;
        L.title_top = 6; L.title_font = &font_plex_12;
        L.rows_top = 24; L.rows_pitch = 15;
        L.rows_font = &font_plex_12;   // "Bateria (cargando)" no cabe en 16
        L.hint_top = 216; L.hint_font = &font_plex_12;
        L.st_top = 226;   L.st_font = &font_plex_12;
    }

    L.card_w = L.scr_w - 2 * L.card_x;
}

// ---- Widgets (lazy: solo existen tras el primer sys_show) -----------------
static lv_obj_t *overlay;
static lv_obj_t *header_lbl;
static lv_obj_t *card1, *card2;
static lv_obj_t *lbl_ver, *lbl_board, *lbl_hero, *lbl_ota, *lbl_act;
static lv_obj_t *bar, *lbl_sub;
static lv_obj_t *lbl_title;
static lv_obj_t *row_lbl[4], *row_val[4];
static lv_obj_t *lbl_hint, *lbl_state;

// ---- Estado ----------------------------------------------------------------
static bool     s_open = false;
static bool     s_built = false;
static bool     s_armed = false;      // primer PWR recibido, esperando confirmacion
static char     s_arm_ver[16] = "";
static uint32_t s_arm_deadline = 0;

// Lo ultimo pintado en LVGL: sys_tick solo toca LVGL cuando lo recien leido
// difiere de esto (SPEC seccion 9, redraw-on-change).
struct SysPaint {
    char hero[24];
    char ota[64];
    char act[48];
    bool act_on;
    bool bar_on;
    int  bar_val;
    char sub[56];
    char batt_lab[24];
    char batt_val[16];
    uint32_t batt_c;
    char ble[24];
    uint32_t ble_c;
    char wifi[48];
    uint32_t wifi_c;
    char portal[16];   // fila tocable: "Abrir" (apagado) / "Cerrar" (activo)
    char hint[80];
    char state[64];
    char board[32];
};
static SysPaint s_painted;
static bool     s_have_paint = false;

// Flags de logica que no son texto pero deciden que area se muestra.
// (Van dentro de SysPaint como act_on/bar_on; el resto ya se refleja en
// las cadenas, asi que paint_same() no necesita nada mas.)
struct SysLogic {
    bool portal;
    bool live;
    bool downloading;
};

// ---- Formato (SPEC seccion 6) ----------------------------------------------

static void fmt_size(long bytes, char *out, size_t n) {
    if (bytes < 0) bytes = 0;
    if (bytes >= 1000000L)
        snprintf(out, n, "%.1f MB", (double)bytes / 1000000.0);
    else
        snprintf(out, n, "%lu KB", (unsigned long)((bytes + 500) / 1000));
}

static void fmt_rate(uint32_t bps, char *out, size_t n) {
    // ota_pull guarda bytes/s en rate_bps (total*1000/elapsed); se muestra
    // en KB/s o MB/s. Tasa 0 = desconocida: cadena vacia.
    if (bps == 0) { out[0] = '\0'; return; }
    if (bps >= 1000000u)
        snprintf(out, n, "%.1f MB/s", (double)bps / 1000000.0);
    else
        snprintf(out, n, "%lu KB/s", (unsigned long)((bps + 500) / 1000));
}

// Edad del ultimo resultado: "ahora mismo" (<60 s) o "hace 35 s, 21 min,
// 3 h 4 min" (mismo vocabulario que la pantalla OpenCode, glosario).
static void fmt_age(uint32_t ms, uint32_t now, char *out, size_t n) {
    uint32_t s = (now - ms) / 1000;
    if (s < 60) {
        strlcpy(out, "ahora mismo", n);
    } else if (s < 3600) {
        snprintf(out, n, "hace %lu s", (unsigned long)s);
        // "hace 35 s" usa segundos solo por debajo de 2 min; si no, minutos.
        if (s >= 120) snprintf(out, n, "hace %lu min", (unsigned long)(s / 60));
    } else {
        snprintf(out, n, "hace %lu h %lu min",
                 (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));
    }
}

// SSID truncado a ~18 + '.' (patron fmt_sym del portafolio, SPEC seccion 9).
static void trunc_ssid(const char *ssid, char *out, size_t n) {
    if (strlen(ssid) <= SYS_SSID_MAX) { strlcpy(out, ssid, n); return; }
    snprintf(out, n, "%.*s.", SYS_SSID_MAX, ssid);
}

// Codigo corto de error del snapshot -> cadena ES exacta (SPEC seccion 8,
// glosario: resto -> "No se actualizo").
static const char *err_es(const char *err) {
    if (!err || !err[0]) return "No se actualizó";
    if (strcmp(err, "timeout") == 0)     return "Sin respuesta";
    if (strcmp(err, "tls_fail") == 0)    return "Error TLS";
    if (strcmp(err, "http_404") == 0)    return "Sin versión";
    if (strcmp(err, "no_wifi") == 0)     return "Sin WiFi";
    if (strcmp(err, "busy") == 0)        return "Ocupado";
    if (strcmp(err, "battery_low") == 0) return "Batería baja";
    return "No se actualizó";
}

// ---- Lectura en vivo --------------------------------------------------------
// Lee TODAS las APIs (SPEC seccion 6) y formatea a `p` sin tocar LVGL.
static void read_live(SysPaint *p, SysLogic *lg, uint32_t now) {
    memset(p, 0, sizeof(*p));

    strlcpy(p->hero, ota_version(), sizeof(p->hero));
    strlcpy(p->board, board_caps().id, sizeof(p->board));

    // BLE (ble_get_state)
    switch (ble_get_state()) {
    case BLE_STATE_CONNECTED:
        strlcpy(p->ble, "Conectado", sizeof(p->ble));
        p->ble_c = 0x2ee88a;
        break;
    case BLE_STATE_ADVERTISING:
        strlcpy(p->ble, "Disponible", sizeof(p->ble));
        p->ble_c = 0xfab283;
        break;
    default:
        strlcpy(p->ble, "Desconectado", sizeof(p->ble));
        p->ble_c = 0xe06c75;
        break;
    }

    // WiFi: portal > STA > apagado
    lg->portal = portal_is_active();
    if (lg->portal) {
        strlcpy(p->wifi, "Portal 192.168.4.1", sizeof(p->wifi));
        p->wifi_c = 0x2ee88a;
    } else if (ota_wifi_connected()) {
        char ssid[SYS_SSID_MAX + 2];
        trunc_ssid(ota_stored_ssid(), ssid, sizeof(ssid));
        if (ssid[0])
            snprintf(p->wifi, sizeof(p->wifi), "%s %d dBm", ssid, ota_wifi_rssi());
        else
            snprintf(p->wifi, sizeof(p->wifi), "%d dBm", ota_wifi_rssi());
        p->wifi_c = 0x2ee88a;
    } else {
        strlcpy(p->wifi, "Apagado", sizeof(p->wifi));
        p->wifi_c = 0xe06c75;
    }
    // Fila tocable del portal: el valor es la accion (SPEC seccion 8).
    strlcpy(p->portal, lg->portal ? "Cerrar" : "Abrir", sizeof(p->portal));

    // Bateria: "78%"; etiqueta "Bateria (cargando)" si carga. pct<0 es lectura
    // ausente: "-" ASCII (el em-dash de la SPEC no existe en los cortes Plex
    // y la seccion 8 lo prohibe).
    int pct = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct < 0) {
        strlcpy(p->batt_lab, "Batería", sizeof(p->batt_lab));
        strlcpy(p->batt_val, "-", sizeof(p->batt_val));
        p->batt_c = 0x8c8c8c;
    } else {
        strlcpy(p->batt_lab, charging ? "Batería (cargando)" : "Batería",
                sizeof(p->batt_lab));
        snprintf(p->batt_val, sizeof(p->batt_val), "%d%%", pct);
        p->batt_c = charging ? 0x2ee88a : 0xeeeeee;
    }

    // OTA: prioridad vivo > resultado > "Sin comprobar" (SPEC seccion 6).
    bool pull_active = ota_pull_is_active();
    const char *pst = ota_pull_state_name();
    bool hybrid = ota_is_active();
    OtaUiSnapshot snap;
    ota_pull_ui_snapshot(&snap);
    bool has_creds = portal_has_creds();

    lg->live = false;
    lg->downloading = false;
    if (pull_active) {
        lg->live = true;
        if (strcmp(pst, "download") == 0) {
            if (snap.pct >= 0) {
                lg->downloading = true;
                char rate[16];
                fmt_rate(snap.rate_bps, rate, sizeof(rate));
                if (rate[0])
                    snprintf(p->ota, sizeof(p->ota), "Descargando %s" SYS_ELLIPSIS " %d%%",
                             rate, snap.pct);
                else
                    // Tasa 0: solo barra + % (SPEC seccion 6).
                    snprintf(p->ota, sizeof(p->ota), "Descargando" SYS_ELLIPSIS " %d%%",
                             snap.pct);
            } else {
                strlcpy(p->ota, "Descargando" SYS_ELLIPSIS, sizeof(p->ota));
            }
        } else if (strcmp(pst, "verify") == 0) {
            strlcpy(p->ota, "Verificando" SYS_ELLIPSIS, sizeof(p->ota));
        } else if (strcmp(pst, "reboot") == 0 || strcmp(pst, "activate") == 0) {
            strlcpy(p->ota, "Reiniciando" SYS_ELLIPSIS, sizeof(p->ota));
        } else {
            // join/sntp/manifest
            strlcpy(p->ota, "Comprobando" SYS_ELLIPSIS, sizeof(p->ota));
        }
    } else if (hybrid) {
        lg->live = true;
        strlcpy(p->ota, "OTA en curso", sizeof(p->ota));
    } else {
        switch ((ota_ui_result_t)snap.result) {
        case OTA_UI_UP_TO_DATE:
            strlcpy(p->ota, "Al día", sizeof(p->ota));
            break;
        case OTA_UI_AVAILABLE: {
            char size[16];
            fmt_size(snap.size, size, sizeof(size));
            snprintf(p->ota, sizeof(p->ota), "Nueva: %s " SYS_MIDDOT " %s",
                     snap.ver, size);
            break;
        }
        case OTA_UI_ERROR:
            strlcpy(p->ota, err_es(snap.err), sizeof(p->ota));
            break;
        case OTA_UI_REBOOTING:
            strlcpy(p->ota, "Reiniciando" SYS_ELLIPSIS, sizeof(p->ota));
            break;
        case OTA_UI_UNKNOWN:
        default:
            // Sin credenciales no hay nada comprobable (guion ASCII, SPEC 8).
            strlcpy(p->ota, has_creds ? "Sin comprobar" : "Sin WiFi - no comprobable",
                    sizeof(p->ota));
            break;
        }
    }

    // Caja de accion o barra (card 1): con portal u OTA en vivo no hay accion
    // (la radio esta ocupada o la pantalla ya muestra progreso).
    p->act_on = false;
    p->bar_on = false;
    if (lg->downloading) {
        p->bar_on = true;
        p->bar_val = snap.pct < 0 ? 0 : (snap.pct > 100 ? 100 : snap.pct);
        char size[16];
        fmt_size(snap.size, size, sizeof(size));
        if (snap.eta_s > 0)
            snprintf(p->sub, sizeof(p->sub), "quedan ~%lu s " SYS_MIDDOT " %s",
                     (unsigned long)snap.eta_s, size);
        else
            strlcpy(p->sub, size, sizeof(p->sub));
    } else if (!lg->portal && !lg->live) {
        p->act_on = true;
        if (!has_creds) {
            strlcpy(p->act, "PWR: abrir portal WiFi", sizeof(p->act));
        } else if (s_armed) {
            snprintf(p->act, sizeof(p->act), "¿Aplicar %s? PWR confirma", s_arm_ver);
        } else if ((ota_ui_result_t)snap.result == OTA_UI_AVAILABLE) {
            strlcpy(p->act, "PWR: aplicar actualización", sizeof(p->act));
        } else {
            strlcpy(p->act, "PWR: comprobar ahora", sizeof(p->act));
        }
    }

    // Hint (y410): los tres patrones de la SPEC seccion 8.
    if (s_armed) {
        strlcpy(p->hint, "PWR: confirmar " SYS_MIDDOT " Toque: cerrar", sizeof(p->hint));
    } else if (lg->downloading) {
        snprintf(p->hint, sizeof(p->hint), "No apagues " SYS_MIDDOT " %d%%", p->bar_val);
    } else if (lg->live) {
        strlcpy(p->hint, "No apagues " SYS_MIDDOT " actualizando", sizeof(p->hint));
    } else if (lg->portal) {
        // Radio ocupada por el portal: aqui PWR es brillo (SPEC seccion 3.4).
        strlcpy(p->hint, "Toque: cerrar " SYS_MIDDOT " PWR: brillo", sizeof(p->hint));
    } else {
        snprintf(p->hint, sizeof(p->hint), "Toque: cerrar " SYS_MIDDOT " %s", p->act);
    }

    // Estado (y448): ultimo resultado o la edad de la comprobacion.
    if (snap.ms == 0) {
        strlcpy(p->state, "Sin comprobar", sizeof(p->state));
    } else {
        char age[24];
        fmt_age(snap.ms, now, age, sizeof(age));
        snprintf(p->state, sizeof(p->state), "Última comprobación %s", age);
    }
}

// ---- Builders (patron ui_portfolio.cpp) -------------------------------------

static lv_obj_t *make_label(lv_obj_t *parent, int16_t x, int16_t y,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(l, 0, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);   // una sola linea
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);         // el tap lo cierra ui.cpp
    return l;
}

static lv_obj_t *make_card(lv_obj_t *parent, int16_t y, int16_t h, int16_t radius) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, L.card_x, y);
    lv_obj_set_size(p, L.card_w, h);
    lv_obj_set_style_bg_color(p, SYS_SURFACE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
    return p;
}

static void place_right(lv_obj_t *l, int16_t inset, int16_t top) {
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -inset, top);
}

// El tap sobre la fila del portal alterna el SoftAP sin cerrar el overlay:
// la fila NO burbujea (sin EVENT_BUBBLE su CLICKED no llega al sys_gesture_cb
// de ui.cpp que cierra) y lleva handler CLICKED propio.
static void sys_portal_row_cb(lv_event_t *e);

static void set_vis(lv_obj_t *l, bool on) {
    if (!l) return;
    if (on) lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
}

// Construye los widgets una sola vez (lazy init, SPEC seccion 9).
static void ensure_built(void) {
    if (s_built) return;
    compute_layout(board_caps());

    // La bateria es de ui.cpp y se queda arriba a la derecha: el header va a
    // la izquierda y no pinta nada en esa esquina.
    overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, L.scr_w, L.scr_h);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_EVENT_BUBBLE);

    header_lbl = make_label(overlay, L.header_x, L.header_top, L.header_font, SYS_MUTED);
    lv_label_set_text(header_lbl, "Command Center");

    card1 = make_card(overlay, L.c1_y, L.c1_h, L.c1_radius);
    card2 = make_card(overlay, L.c2_y, L.c2_h, L.c2_radius);

    lbl_ver = make_label(card1, L.pad, L.label_top, L.label_font, SYS_MUTED);
    lv_label_set_text(lbl_ver, "Versión");
    set_vis(lbl_ver, L.has_label);
    lbl_board = make_label(card1, 0, L.board_top, L.board_font, SYS_MUTED);
    place_right(lbl_board, L.board_ri, L.board_top);
    lbl_hero = make_label(card1, L.pad, L.hero_top, L.hero_font, SYS_TEXT);
    lbl_ota  = make_label(card1, L.pad, L.ota_top, L.ota_font, SYS_TEXT);
    lbl_act  = make_label(card1, L.pad, L.act_top, L.act_font, SYS_MUTED);

    bar = lv_bar_create(card1);
    lv_obj_set_pos(bar, L.pad, L.bar_y);
    lv_obj_set_size(bar, L.card_w - 2 * L.pad, L.bar_h);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, SYS_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, SYS_SUCCESS, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, L.bar_h / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, L.bar_h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    lbl_sub = make_label(card1, L.pad, L.sub_top, L.sub_font, SYS_MUTED);

    lbl_title = make_label(card2, L.pad, L.title_top, L.title_font, SYS_MUTED);
    lv_label_set_text(lbl_title, "Radios y batería");
    static const char *const names[4] = { "BLE", "WiFi", "Batería", "Portal WiFi" };
    for (int i = 0; i < 4; i++) {
        int16_t top = (int16_t)(L.rows_top + i * L.rows_pitch);
        row_lbl[i] = make_label(card2, L.pad, top, L.rows_font, SYS_MUTED);
        lv_label_set_text(row_lbl[i], names[i]);
        row_val[i] = make_label(card2, 0, top, L.rows_font, SYS_TEXT);
        place_right(row_val[i], L.pad, top);
    }
    // La 4a fila es tocable: CLICKABLE + CLICKED propio, SIN burbuja para que
    // el tap no cierre el overlay (ver sys_portal_row_cb).
    for (int i = 0; i < 2; i++) {
        lv_obj_t *o = (i == 0) ? row_lbl[3] : row_val[3];
        lv_obj_clear_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(o, sys_portal_row_cb, LV_EVENT_CLICKED, NULL);
    }

    lbl_hint = make_label(overlay, L.header_x, L.hint_top, L.hint_font, SYS_MUTED);
    lbl_state = make_label(overlay, L.header_x, L.st_top, L.st_font, SYS_MUTED);

    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    s_built = true;
}

// ---- Pintado (solo con datos ya leidos) --------------------------------------

static void set_text(lv_obj_t *l, const char *cur, char *prev, size_t n) {
    if (strcmp(cur, prev) != 0) {
        lv_label_set_text(l, cur);
        strlcpy(prev, cur, n);
    }
}

static void set_color(lv_obj_t *l, uint32_t hex, uint32_t *prev) {
    if (hex != *prev) {
        *prev = hex;
        lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    }
}

static bool paint_same(const SysPaint *a, const SysPaint *b) {
    return strcmp(a->hero, b->hero) == 0
        && strcmp(a->ota, b->ota) == 0
        && strcmp(a->act, b->act) == 0
        && a->act_on == b->act_on
        && a->bar_on == b->bar_on
        && a->bar_val == b->bar_val
        && strcmp(a->sub, b->sub) == 0
        && strcmp(a->batt_lab, b->batt_lab) == 0
        && strcmp(a->batt_val, b->batt_val) == 0
        && a->batt_c == b->batt_c
        && strcmp(a->ble, b->ble) == 0
        && a->ble_c == b->ble_c
        && strcmp(a->wifi, b->wifi) == 0
        && a->wifi_c == b->wifi_c
        && strcmp(a->portal, b->portal) == 0
        && strcmp(a->hint, b->hint) == 0
        && strcmp(a->state, b->state) == 0
        && strcmp(a->board, b->board) == 0;
}

// Dibuja `p` tal cual: no lee ninguna API, solo empuja a LVGL lo que cambio.
static void paint(const SysPaint *p) {
    set_text(lbl_board, p->board, s_painted.board, sizeof(s_painted.board));
    set_text(lbl_hero, p->hero, s_painted.hero, sizeof(s_painted.hero));
    set_text(lbl_ota, p->ota, s_painted.ota, sizeof(s_painted.ota));

    bool had_act = !lv_obj_has_flag(lbl_act, LV_OBJ_FLAG_HIDDEN);
    if (p->act_on != had_act) set_vis(lbl_act, p->act_on);
    if (p->act_on) set_text(lbl_act, p->act, s_painted.act, sizeof(s_painted.act));

    bool had_bar = !lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN);
    if (p->bar_on != had_bar) {
        set_vis(bar, p->bar_on);
        set_vis(lbl_sub, p->bar_on);
    }
    if (p->bar_on) {
        if (p->bar_val != s_painted.bar_val) {
            s_painted.bar_val = p->bar_val;
            lv_bar_set_value(bar, p->bar_val, LV_ANIM_OFF);
        }
        set_text(lbl_sub, p->sub, s_painted.sub, sizeof(s_painted.sub));
    }

    set_text(row_lbl[2], p->batt_lab, s_painted.batt_lab, sizeof(s_painted.batt_lab));
    set_text(row_val[2], p->batt_val, s_painted.batt_val, sizeof(s_painted.batt_val));
    set_color(row_val[2], p->batt_c, &s_painted.batt_c);
    set_text(row_val[0], p->ble, s_painted.ble, sizeof(s_painted.ble));
    set_color(row_val[0], p->ble_c, &s_painted.ble_c);
    set_text(row_val[1], p->wifi, s_painted.wifi, sizeof(s_painted.wifi));
    set_color(row_val[1], p->wifi_c, &s_painted.wifi_c);
    set_text(row_val[3], p->portal, s_painted.portal, sizeof(s_painted.portal));

    set_text(lbl_hint, p->hint, s_painted.hint, sizeof(s_painted.hint));
    set_text(lbl_state, p->state, s_painted.state, sizeof(s_painted.state));
}

// ======== API publica =========================================================

void sys_init_lazy(void) {
    // Sin objetos LVGL: solo deja la geometria lista y limpia el estado.
    compute_layout(board_caps());
    s_open = false;
    s_armed = false;
    s_arm_ver[0] = '\0';
    memset(&s_painted, 0, sizeof(s_painted));
    s_have_paint = false;
}

void sys_show(void) {
    ensure_built();
    // Topmost de la pantalla activa: si se navego con el overlay cerrado, se
    // reparenta; los hijos usan coordenadas absolutas dentro del overlay.
    lv_obj_t *scr = lv_screen_active();
    if (lv_obj_get_parent(overlay) != scr) lv_obj_set_parent(overlay, scr);
    lv_obj_move_foreground(overlay);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    s_open = true;
    s_have_paint = false;   // repintado completo al abrir
    sys_tick();
}

void sys_hide(void) {
    if (overlay) lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    s_open = false;
    s_armed = false;        // cerrar desarma la confirmacion (SPEC 3.2)
    s_arm_ver[0] = '\0';
}

bool sys_is_open(void) {
    return s_open;
}

void sys_tick(void) {
    if (!s_open || !overlay) return;
    uint32_t now = millis();
    // Vence la ventana de confirmacion del doble PWR.
    if (s_armed && (int32_t)(now - s_arm_deadline) >= 0) {
        s_armed = false;
        s_arm_ver[0] = '\0';
    }
    SysPaint cur;
    SysLogic lg;
    read_live(&cur, &lg, now);
    (void)lg;
    if (s_have_paint && paint_same(&cur, &s_painted)) return;   // sin cambios
    paint(&cur);
    s_have_paint = true;
}

// PWR corto con el overlay abierto (SPEC seccion 5). Solo encola o cambia el
// armado; el repintado lo hace sys_tick() con datos ya leidos.
void sys_pwr_action(void) {
    if (!s_open) return;
    if (portal_is_active()) { brightness_cycle(); return; }   // radio ocupada
    if (ota_pull_is_active() || ota_is_active()) return;      // ya muestra progreso
    if (!portal_has_creds()) { portal_start(); return; }      // sin WiFi -> portal
    OtaUiSnapshot snap;
    ota_pull_ui_snapshot(&snap);
    if ((ota_ui_result_t)snap.result == OTA_UI_AVAILABLE && !s_armed) {
        s_armed = true;
        strlcpy(s_arm_ver, snap.ver, sizeof(s_arm_ver));
        s_arm_deadline = millis() + SYS_ARM_MS;
        s_have_paint = false;
        sys_tick();
        return;
    }
    if (s_armed) {
        s_armed = false;
        s_arm_ver[0] = '\0';
        ota_pull_request_apply();
        s_have_paint = false;
        sys_tick();
        return;
    }
    ota_pull_request_check();
    s_have_paint = false;
    sys_tick();
}

// Tap en la 4a fila de card2: alterna el portal sin cerrar el overlay (el
// objeto no burbujea, asi que sys_gesture_cb no ve este CLICKED). El tick
// redibuja al cambiar portal_is_active() via SysPaint.portal.
static void sys_portal_row_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_open) return;
    if (portal_is_active()) portal_stop();
    else                    portal_start();
    s_have_paint = false;
    sys_tick();
}
