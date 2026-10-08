#include "ui.h"
#include "splash.h"
#include "ui_opencode.h"
#include "ui_portfolio.h"
#include "ui_sys.h"
#include "oc_splash.h"
#include "ota.h"
#include "ota_pull.h"
#include "usage_pair.h"
#include <lvgl.h>
#include <time.h>
#include <string.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "hal/board_caps.h"

// El traspaso del estado de OTA de abajo usa una sección crítica sobre el
// hardware; el simulador nativo es de un solo hilo y no tiene FreeRTOS, así que
// el bloqueo se compila allí.
#ifndef BOARD_SIM
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

// Fuentes propias (escaladas a 314 PPI, ~1,9x desde las 165 PPI originales)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Valores de diseño calculados a partir de la geometría de la placa activa. Se
// rellenan una sola vez en ui_init() y se tratan como constantes durante el resto
// del programa. Añadir un tamaño de pantalla nuevo significa extender
// compute_layout() con otro punto de ruptura, nunca editar las funciones de abajo
// que construyen las pantallas.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Pantalla de uso
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // título de pantalla / reloj
    const lv_font_t* pct_font;       // número grande de porcentaje
    const lv_font_t* ent_pct_font;   // número de gasto (enterprise)
    const lv_font_t* pill_font;      // píldora de "Actual" / "Semanal"
    const lv_font_t* reset_font;     // línea "Se renueva en ..."
    const lv_font_t* pace_font;      // línea de ritmo enterprise ("Ritmo bajo/En ritmo/Ritmo alto")
    const lv_font_t* anim_font;      // línea de estado animada
    int16_t anim_y;                  // desplazamiento de la línea de estado desde abajo
    bool    small_icons;             // logo de 40 px + batería de 24 px (frente a 80/48) en pantallas pequeñas
    int16_t title_nudge;             // desplazamiento en x del título que compensa el logo de la esquina
    int16_t logo_y;                  // borde superior del logo
    int16_t batt_y;                  // borde superior del icono de batería
    int16_t batt_w;                  // ancho del icono de batería, para las cuentas de posición

    // Pantalla de emparejamiento / de reposo
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // tamaño de la criatura dormida en la pantalla de reposo

    // Vista de código de emparejamiento del backend (fase 2): desplazamientos de
    // título / código / pista desde arriba del área de contenido, más la fuente
    // grande del código.
    int16_t paircode_y1, paircode_y2, paircode_y3;
    const lv_font_t* paircode_code_font;

    // Indicador de página (un punto por pantalla del ciclo actual)
    int16_t dots_y;                  // fila central de los puntos

    // Pantalla de Bluetooth
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Elige los valores de diseño a partir de las dimensiones en píxeles de la placa
// activa. Las dos placas existentes caen justo en los dos puntos de ruptura de
// abajo; los portes nuevos heredan el más cercano: se ve bien, puede necesitar
// una pasada de pulido para la alineación perfecta al píxel, pero nunca impide
// que el porte arranque.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Valores compartidos por los dos puntos de ruptura originales; la rama
    // pequeña de abajo los sobrescribe por completo.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.anim_font    = &font_mono_32;
    L.anim_y = -15;
    L.small_icons = false;
    L.title_nudge = 16;
    L.logo_y = L.title_y - 10;
    L.batt_y = L.title_y;
    L.batt_w = ICON_BATTERY_W;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;
    L.paircode_y1 = 48;
    L.paircode_y2 = 138;
    L.paircode_y3 = 214;
    L.paircode_code_font = &font_styrene_48;

    if (c.height >= 460) {
        // Diseño grande, ajustado para 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
        L.dots_y = 472;   // Anexo A de SPEC.md
    } else if (c.height >= 300) {
        // Diseño compacto, ajustado para 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
        L.dots_y = 440;
        L.paircode_y1 = 40;
        L.paircode_y2 = 116;
        L.paircode_y3 = 182;
        L.paircode_code_font = &font_styrene_28;
    } else {
        // Diseño pequeño, ajustado para 240x240 (LCD-1.54 y TFT cuadrados
        // similares). Todo se encoge: fuentes dos pasos más chicas, paneles a
        // media altura y el logo y la batería de la esquina pasan a los recursos
        // pequeños de 40/24 px.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        // Centrar la línea de estado en la franja bajo el panel semanal; pegada
        // al borde inferior se lee como un espaciado disparejo.
        L.anim_y = -10;
        L.small_icons = true;
        L.title_nudge = 8;
        L.logo_y = 2;
        L.batt_y = 10;
        L.batt_w = ICON_BATTERY_SMALL_W;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
        L.dots_y = 234;
        L.paircode_y1 = 6;
        L.paircode_y2 = 48;
        L.paircode_y3 = 100;
        L.paircode_code_font = &font_styrene_20;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Paleta de marca de Anthropic; los tokens de diseño viven en theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Widgets de la pantalla de uso (la única vista que no es splash) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Reloj alimentado por el daemon: época base (segundos del reloj local) + el
// lv_tick en el que llegó, para que el título avance solo entre payloads de 60 s.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 o 24, tomado del payload del daemon
static int      clock_last_min = -1;   // último minuto dibujado; evita redibujar el título en cada tick
static lv_obj_t* usage_group;   // los dos paneles de uso, visibles con conexión
static lv_obj_t* pair_group;    // aviso de emparejamiento, visible sin conexión
static lv_obj_t* paircode_group; // código de emparejamiento del backend, visible sin vincular
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Widgets solo para enterprise dentro de panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" en una fuente más chica
static lv_obj_t* lbl_spending_desc = nullptr;     // "de tu presupuesto mensual"
static lv_obj_t* lbl_spending_status = nullptr;   // "Ritmo bajo" / "En ritmo" / "Ritmo alto"
static lv_obj_t* lbl_anim;      // línea de estado: estado de conexión + reposo fantasioso

// Vista de código de emparejamiento del backend (fase 2): el código grande y
// una pista de una sola línea.
static lv_obj_t* lbl_paircode_code;
static lv_obj_t* lbl_paircode_hint;

// ---- Indicador de batería (compartido, encima) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // vacía, baja, media, llena, cargando

// ---- Indicador de página (SPEC.md §6) ----
// Un punto de 6 px por pantalla del ciclo actual (2, y 4 cuando entran las
// pantallas de OpenCode, 5 cuando entra también el portfolio), separados 8 px y
// centrados en L.dots_y. Se muestran 1,5 s después de cada cambio de pantalla,
// por encima de todo, y se vuelven a ocultar.
#define PAGE_DOT_D     6
#define PAGE_DOT_GAP   8
#define PAGE_DOT_MAX   5
#define PAGE_DOTS_MS   1500
#define COL_DOT_ON     lv_color_hex(0xeeeeee)
#define COL_DOT_OFF    lv_color_hex(0x484848)
static lv_obj_t* dots_root;
static lv_obj_t* dots[PAGE_DOT_MAX];
static int       dots_lit = -1;       // índice del punto resaltado (-1 = nunca se mostró)
static uint32_t  dots_until_ms = 0;   // lv_tick en el que la fila se oculta sola

// ---- Frescura de los datos en vivo y qué subvista de uso mostrar ----
// los paneles de uso cuando los datos fluyen, una pantalla de reposo "Zzz"
// cuando el host está conectado pero no llegó ninguna actualización de uso
// dentro de DATA_FRESH_MS, y el aviso de emparejamiento cuando BLE está caído.
// Se reevalúa en cada vuelta de ui_tick_anim().
static lv_obj_t* idle_group;            // la pantalla de reposo "Zzz"
static uint32_t  last_data_ms = 0;      // lv_tick de la última actualización de uso válida
static bool      data_received = false; // alguna actualización válida desde el arranque
static bool      data_ok = true;        // indicador ok del último payload; un latido {"ok":false} = "sin datos frescos"
// Uso que NO llegó por el enlace del daemon BLE (sino por el pull del backend
// WiFi, usage_pull.cpp). Se refresca cada USAGE_POLL_S (300 s por defecto), mucho
// más lento que la ventana BLE de 90 s, y existe justamente cuando BLE está
// caído o en reposo, así que recibe su propia ventana de frescura, más larga:
// tres intervalos de pull por defecto, suficientes para capear un par de pulls
// fallidos.
static uint32_t  ext_data_ms = 0;       // lv_tick de la última actualización externa
static bool      ext_received = false;  // alguna actualización externa desde el arranque
static const uint32_t EXT_FRESH_MS = 900000;   // 15 min
static int       view_state = -1;       // -1 desconocido / 0 emparejando / 1 reposo / 2 uso
static const uint32_t DATA_FRESH_MS = 90000;  // el uso cuenta como "en vivo" dentro de esta ventana (el daemon envía ~60 s)

// ---- Compartido ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // estado de conexión BLE en caché
static uint32_t connected_at_ms = 0;       // cuándo entramos por última vez en CONNECTED (pausa de "Conectado")

// Estado de la animación
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

// ---- Línea transitoria de OTA por pull (DESIGN §12) ----
// ui_ota_status() la llama el worker del pull de OTA, que nunca debe tocar LVGL.
// Solo copia el texto + el porcentaje y estampa aquí una fecha límite; el
// lv_label_set_text lo hace ui_tick_anim() (tarea del loop). La línea se borra
// sola tras unos segundos, así que un panel oscuro nunca frena el motor.
#define OTA_STATUS_MAX   32
#define OTA_STATUS_MS    3000
static char     ota_status_text[OTA_STATUS_MAX];
static int      ota_status_pct   = -1;
static uint32_t ota_status_until = 0;       // fecha límite en lv_tick
static bool     ota_status_valid = false;
#ifndef BOARD_SIM
static portMUX_TYPE ota_status_mux = portMUX_INITIALIZER_UNLOCKED;
#define OTA_STATUS_LOCK()   portENTER_CRITICAL(&ota_status_mux)
#define OTA_STATUS_UNLOCK() portEXIT_CRITICAL(&ota_status_mux)
#else
#define OTA_STATUS_LOCK()   ((void)0)
#define OTA_STATUS_UNLOCK() ((void)0)
#endif

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ida y vuelta 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Logrando", "Esclareciendo", "Hojeando",
    "Accionando", "Encantando", "Filosofando",
    "Actualizando", "Vislumbrando", "Elucubrando",
    "Horneando", "Ingeniándoselas", "Pontificando",
    "Boopeando", "Paroleando", "Procesando",
    "Infusionando", "Forjando", "Pululando",
    "Calculando", "Formando", "Barajando",
    "Cerebreando", "Retozando", "Reticulando",
    "Canalizando", "Generando", "Rumiando",
    "Batiendo", "Germinando", "Maquinando",
    "Claudeando", "Eclosionando", "Arrastrando",
    "Aglutinando", "Arreando", "Meneándose",
    "Cavilando", "Graznando", "Desvainando",
    "Combobulando", "Trajinando", "Burbujeando",
    "Computando", "Ideando", "Chafando",
    "Preparando", "Imaginando", "Espeleando",
    "Conjurando", "Incubando", "Girando",
    "Considerando", "Infiriendo", "Guisando",
    "Contemplando", "Bailoteando", "Desentrañando",
    "Cocinando", "Manifestando", "Sintetizando",
    "Elaborando", "Marinando", "Pensando",
    "Creando", "Deambulando", "Cacharreando",
    "Masticando", "Paseando", "Transmutando",
    "Descifrando", "Ponderando", "Desplegando",
    "Deliberando", "Reuniendo", "Desenredando",
    "Determinando", "Meditando", "Vibrando",
    "Descombobulando", "Trasteando", "Vagando",
    "Adivinando", "Filtrando", "Zumbando",
    "Haciendo", "Bamboleándose",
    "Efectuando", "Hechizando",
    "Trabajando", "Forcejeando",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Se renueva en %d min", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Se renueva en %d h %d min", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Se renueva en %d d %d h", mins / 1440, (mins % 1440) / 60);
    }
}

// Declaraciones adelantadas: los callbacks se definen junto a ui_show_screen, abajo
static void global_click_cb(lv_event_t* e);
static void sys_gesture_cb(lv_event_t* e);   // Command Center: gestos a nivel de ciclo

// ======== Indicador de página ========
// Una franja transparente de ancho completo abajo; los puntos nunca se quedan
// con el clic, así que un toque aquí llega y navega igual que un toque en
// cualquier otro lado. Se crea al final para que quede por encima de los
// widgets de todas las pantallas (y de la batería), lo que importa porque la
// franja se superpone con sus márgenes inferiores.
static void build_page_dots(lv_obj_t* parent) {
    dots_root = lv_obj_create(parent);
    lv_obj_set_size(dots_root, L.scr_w, PAGE_DOT_D);
    lv_obj_set_pos(dots_root, 0, L.dots_y - PAGE_DOT_D / 2);
    lv_obj_set_style_bg_opa(dots_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots_root, 0, 0);
    lv_obj_set_style_pad_all(dots_root, 0, 0);
    lv_obj_clear_flag(dots_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dots_root, global_click_cb, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < PAGE_DOT_MAX; i++) {
        lv_obj_t* d = lv_obj_create(dots_root);
        lv_obj_set_size(d, PAGE_DOT_D, PAGE_DOT_D);
        lv_obj_set_style_bg_color(d, COL_DOT_OFF, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(d, 0, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(d, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);   // set_page_dots() los revela
        dots[i] = d;
    }
    lv_obj_add_flag(dots_root, LV_OBJ_FLAG_HIDDEN);   // ui_show_screen() lo revela
}

// Centra la franja en el punto medio de la pantalla y luego la colorea: `n`
// puntos visibles, el punto `lit` resaltado. La x de cada punto se mueve cuando
// el ciclo crece de 2 a 4 a 5 pantallas, así que ambas cosas se recalculan aquí
// en lugar de una sola vez al construir.
static void set_page_dots(int n, int lit) {
    int step = PAGE_DOT_D + PAGE_DOT_GAP;
    int total = n * PAGE_DOT_D + (n - 1) * PAGE_DOT_GAP;
    int x0 = (L.scr_w - total) / 2;
    for (int i = 0; i < PAGE_DOT_MAX; i++) {
        if (i < n) {
            lv_obj_set_pos(dots[i], x0 + i * step, 0);
            lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(dots[i], (i == lit) ? COL_DOT_ON : COL_DOT_OFF, 0);
        } else {
            lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    dots_lit = lit;
}

// Oculta la franja cuando se termina su ventana de 1,5 s. Corre desde
// ui_tick_anim() antes del retorno temprano de cada pantalla, así que caduca en
// todas.
static void tick_page_dots(void) {
    if (!dots_root || dots_lit < 0) return;
    if (lv_obj_has_flag(dots_root, LV_OBJ_FLAG_HIDDEN)) return;
    if ((int32_t)(lv_tick_get() - dots_until_ms) < 0) return;
    lv_obj_add_flag(dots_root, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.pill_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

static void init_battery_icons(void) {
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Pantalla de uso ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Aviso de emparejamiento: se muestra sin conexión para que la pantalla no quede
// vacía y el usuario sepa cómo (re)emparejar. La redacción acompaña el gesto de
// soltar a los 3 segundos.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "Para emparejar");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "mantén pulsado el botón");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "central durante 3 s y suéltalo");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decide
}

// Da formato al código de 8 caracteres como "ABCD-2345" para que se lea mejor. El
// alfabeto (PHASE2-CONTRACT.md §2) no tiene caracteres especiales de HTML ni de
// LVGL, así que el resultado se puede poner tal cual en una etiqueta o en el
// portal.
static void format_pair_code(const char* code, char* buf, size_t len) {
    if (!code || !code[0]) { buf[0] = '\0'; return; }
    if (strlen(code) == 8) snprintf(buf, len, "%.4s-%.4s", code, code + 4);
    else                   snprintf(buf, len, "%s", code);
}

// Vista de código de emparejamiento del backend (fase 2, PHASE2-CONTRACT.md §7).
// Se muestra mientras el dispositivo no tiene token de dispositivo pero ya se
// generó un código; en cuanto se guarda un token vuelve la vista de uso normal
// (ver update_view_state()). Copia el diseño de área de contenido completa de
// build_pair_group() para que el código se lea a un brazo de distancia, y a
// propósito muestra un código —no el aviso BLE— para que el primer paso de la
// puesta en marcha sea único y sin ambigüedades.
static void build_pair_code_group(lv_obj_t* parent) {
    paircode_group = lv_obj_create(parent);
    lv_obj_set_size(paircode_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(paircode_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(paircode_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(paircode_group, 0, 0);
    lv_obj_set_style_pad_all(paircode_group, 0, 0);
    lv_obj_clear_flag(paircode_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(paircode_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(paircode_group);
    lv_label_set_text(l1, "Código");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.paircode_y1);

    lbl_paircode_code = lv_label_create(paircode_group);
    lv_label_set_text(lbl_paircode_code, "");
    lv_obj_set_style_text_font(lbl_paircode_code, L.paircode_code_font, 0);
    lv_obj_set_style_text_color(lbl_paircode_code, COL_ACCENT, 0);
    lv_obj_align(lbl_paircode_code, LV_ALIGN_TOP_MID, 0, L.paircode_y2);

    lbl_paircode_hint = lv_label_create(paircode_group);
    lv_label_set_text(lbl_paircode_hint, "");
    lv_obj_set_style_text_font(lbl_paircode_hint, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_paircode_hint, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_paircode_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_paircode_hint, L.content_w);
    lv_obj_align(lbl_paircode_hint, LV_ALIGN_TOP_MID, 0, L.paircode_y3);

    lv_obj_add_flag(paircode_group, LV_OBJ_FLAG_HIDDEN);
}

// El código de emparejamiento es estado de runtime y puede aparecer o cambiar
// después de construir el grupo, así que las dos etiquetas se refrescan desde
// usage_pair_*() en cada tick mientras la vista está visible. Solo escribe cuando
// un valor cambió de verdad.
static void tick_pair_code(void) {
    if (!paircode_group || !lbl_paircode_code || !lbl_paircode_hint) return;

    char code[16];
    format_pair_code(usage_pair_code(), code, sizeof(code));

    const char* st = usage_pair_state();
    const char* hint = (st && strcmp(st, "error") == 0)
        ? "Sin respuesta del servidor - reintentando"
        : "Introduce este código en la herramienta para vincular";

    static char last_code[16];
    static char last_hint[64];
    if (strcmp(code, last_code) != 0) {
        lv_label_set_text(lbl_paircode_code, code);
        snprintf(last_code, sizeof(last_code), "%s", code);
    }
    if (strcmp(hint, last_hint) != 0) {
        lv_label_set_text(lbl_paircode_hint, hint);
        snprintf(last_hint, sizeof(last_hint), "%s", hint);
    }
}

// Pantalla de reposo "Zzz": se muestra cuando el host está conectado pero no
// llegó ninguna actualización de uso hace poco (token vencido, daemon caído, host
// dormido…). Ocupa toda la pantalla, igual que el aviso de emparejamiento, para
// no dibujar nunca cifras de hace horas como si estuvieran en vivo.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Una criatura descansando, encogida (la animación oficial del paseo en
    // nube), va entre el encabezado y la línea de estado; la línea animada
    // "Escuchando…" pone las palabras, así que aquí no hace falta texto extra.
    lv_obj_t* creature = splash_mini_create(idle_group, "cloud", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decide
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Consumo");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // El desplazamiento compensa el logo de la esquina izquierda; es más chico
    // en las pantallas pequeñas, donde el logo es de 40 px y el icono de
    // batería está más cerca.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    // Los paneles de uso (visibles con conexión) viven en un grupo transparente
    // de tamaño completo para poder mostrarlos u ocultarlos contra el aviso de
    // emparejamiento como una sola unidad.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Actual",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Superposiciones solo para enterprise dentro de panel_session; ocultas hasta que lleguen datos enterprise
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "de tu presupuesto mensual");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Semanal",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor activado para que la caja de periodo de enterprise pueda colorear el ritmo y la renovación por separado
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);
    build_pair_code_group(usage_container);

    // Línea de estado, siempre visible en la vista de uso. La maneja ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);
}

// ======== API pública ========

void ui_init(void) {
    compute_layout(board_caps());
    sys_init_lazy();   // Command Center: barato, sin LVGL (SPEC §9)

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#ifndef BOARD_HAS_PSRAM
    // Mascota quieta de la esquina (ver clawd_still.h); la animada necesita PSRAM.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
#endif
    init_battery_icons();

    init_usage_screen(scr);
    splash_init(scr);
    // La pantalla de uso de OpenCode se construye de forma perezosa, en el primer
    // ui_show_screen, igual que el portfolio de abajo: construirla aquí con todo
    // lo demás agota el heap LVGL y no deja sitio al overlay del Command Center
    // (seis pantallas de objetos de golpe en 480x480). La pantalla solo entra al
    // ciclo cuando llega un payload, que es justo cuando el usuario la encendió;
    // mientras tanto oc_usage_update() guarda la copia y redraw() no hace nada.
    // La pantalla del portfolio se construye de forma perezosa, en el primer
    // ui_show_screen, y no aquí: cinco pantallas de objetos LVGL de golpe agotan
    // el heap interno en las placas de 480x480, y el siguiente malloc dentro de
    // esp_intr_alloc hace saltar el canario de pila. La pantalla solo entra al
    // ciclo cuando llega un payload, que es justo cuando el usuario la encendió.

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Mascota de la esquina en la vieja ranura del logo. El Clawd quieto es más
    // bajo que la ranura de 80/40 px que usaba el logo de la chispa; hay que
    // centrarlo verticalmente en esa ranura.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
#ifdef BOARD_HAS_PSRAM
        // Animado: reposa, hace gracias y sale a pasear o a acechar.
        splash_mascot_create(scr, L.margin, top + art_h, L.small_icons ? 2 : 3);
#else
        logo_img = lv_image_create(scr);
        lv_image_set_src(logo_img, &logo_dsc);
        lv_obj_set_pos(logo_img, L.margin, top);
#endif
    }

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - L.batt_w - L.margin, L.batt_y);
    // El tap sobre la batería también burbujea a la pantalla: con el overlay
    // abierto cierra (está al frente con él), sin overlay no hace nada.
    lv_obj_add_flag(battery_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    // Las placas sin telemetría de batería nunca muestran el indicador (según el
    // contrato del HAL; antes todas las placas dibujaban el glifo de batería
    // vacía).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    }

    // Al final, para que los puntos sean los hijos más al frente de la pantalla y
    // se mantengan legibles sobre lo que cada pantalla dibuja en su borde inferior.
    build_page_dots(scr);
    lv_obj_move_foreground(dots_root);

    // Command Center: escucha de gestos a nivel de ciclo (§3.1), no por pantalla.
    lv_obj_add_event_cb(scr, sys_gesture_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, sys_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr, sys_gesture_cb, LV_EVENT_CLICKED, NULL);
}

static void ui_update_impl(const UsageData* data, bool external);

// El camino BLE (main.cpp) y, hoy por hoy, también el pull WiFi. Sin enlace BLE
// la única fuente posible es el pull WiFi, así que una actualización que llega
// desconectado se clasifica como externa automáticamente.
void ui_update(const UsageData* data) {
    ui_update_impl(data, !s_ble_connected);
}

// Punto de entrada explícito del pull WiFi: cuenta como externo incluso con BLE
// conectado (p. ej. si solo está arriba el enlace HID del sistema operativo y
// ningún daemon nos alimenta).
void ui_update_external(const UsageData* data) {
    ui_update_impl(data, true);
}

static void ui_update_impl(const UsageData* data, bool external) {
    if (!data->valid) return;
    data_ok = data->ok;
    if (!data->ok) return;          // un latido {"ok":false} de "sin datos": caer al reposo conservando las últimas cifras
    last_data_ms = lv_tick_get();   // acaba de llegar una actualización real de uso
    data_received = true;
    if (external) {
        ext_data_ms = last_data_ms;
        ext_received = true;
    }

    if (data->clock_epoch > 0) {    // el daemon trajo hora de pared → alimentar el reloj del título
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // el daemon apagó el reloj → devolver el título a "Consumo"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Consumo");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Caja de gasto: etiqueta grande solo con el número + símbolo "%" chico + descripción + ritmo
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Gasto");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, "Actual");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[96];

    // Variables de ritmo que usan los dos bloques enterprise de abajo. Van en la
    // línea recolorada "ritmo - Renueva <fecha>" del panel semanal, así que
    // tienen que caber en el ancho del panel junto a la fecha.
    const char* pace_text = "Ritmo bajo";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // coincide con THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Ritmo alto";        pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "En ritmo";          pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Caja de periodo: % de tiempo + color de ritmo dinámico + etiqueta "Renueva <fecha>"
        lv_label_set_text(lbl_weekly_label, "Periodo");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Renueva %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }
}

// Elige la subpantalla de la vista de uso: el código de emparejamiento del
// backend (sin vincular, fase 2), el aviso de emparejamiento BLE (BLE caído), la
// pantalla de reposo "Zzz" (conectado pero con datos viejos) o los paneles de uso
// en vivo. Solo vuelve a armar el layout cuando el estado cambia de verdad. La
// línea de estado animada se queda visible en todos lados: dice "Escuchando…" en
// la pantalla de reposo y "Emparejando…" en la del código, para que todo se vea
// vivo.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group || !paircode_group) return;
    int v;
    const uint32_t now = lv_tick_get();
    const char* pcode = usage_pair_code();
    if (!usage_pair_has_token() && pcode && pcode[0]) {
        // Emparejamiento por backend (fase 2): todavía no hay token de
        // dispositivo pero ya se generó un código; mostrar el código hasta que el
        // dueño lo apruebe, pase lo que pase con BLE. Se va solo en cuanto
        // usage_pair_has_token() cambia a true.
        v = 3;
    } else if (ext_received && data_ok && (now - ext_data_ms) < EXT_FRESH_MS) {
        v = 2;  // uso en vivo desde el pull WiFi; se muestra en cualquier estado de BLE
    } else if (!s_ble_connected) {
        v = 0;  // aviso de emparejamiento
    } else if (data_received && data_ok && (now - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // uso en vivo (daemon BLE, ventana de 90 s: sin cambios)
    } else {
        v = 1;  // reposo / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(paircode_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group :
                      v == 2 ? usage_group : paircode_group,
                      LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    // Primero el trabajo que no depende de la pantalla: la ventana de 1,5 s del
    // indicador de página tiene que caducar también en el splash y en las dos
    // pantallas de OpenCode, y las de OpenCode y del portfolio mantienen sus
    // propias líneas de estado andando mientras están visibles.
    tick_page_dots();
    sys_tick();   // Command Center: lectura en vivo en cualquier pantalla
    // La batería es de ui.cpp y se conserva arriba a la derecha con el overlay
    // abierto (§3.3): el overlay opaco la taparía, así que al abrir vuelve al
    // frente (cubre la apertura por gesto y la programática del sim).
    {
        static bool sys_open_seen = false;
        bool open = sys_is_open();
        if (open && !sys_open_seen && battery_img) lv_obj_move_foreground(battery_img);
        sys_open_seen = open;
    }
    if (current_screen == SCREEN_OC_USAGE) oc_usage_tick();
    if (current_screen == SCREEN_PORTFOLIO) pf_usage_tick();
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animar la criatura dormida en la pantalla de reposo
    else if (view_state == 3) tick_pair_code();  // mantener el código y la pista al día

    uint32_t now = lv_tick_get();

    // Reloj del título: en cuanto el daemon manda la hora de pared, "Consumo" se
    // reemplaza por la hora en vivo, avanzada en local para que cambie cada minuto
    // entre payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // la época ya es hora local → gmtime la deja tal cual
        if (tmv.tm_min != clock_last_min) {   // reescribir el título solo cuando cambia el minuto
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Texto de estado por prioridad. Los mensajes fantasiosos solo aparecen con
    // conexión y ya asentados. Una OTA en vuelo se apropia de esta línea en lugar
    // de agregar una pantalla (§5): el camino de uso sigue andando, así que la
    // placa es puramente adicional. Tanto ota_is_active() como
    // ota_pull_is_active() están forzados a false en el simulador.
    //
    // Si hay una línea transitoria de OTA por pull viva, le gana a la placa
    // genérica. Su texto lo guardó ui_ota_status() (posiblemente en la tarea del
    // pull); la etiqueta se escribe aquí, en la tarea del loop, para que ninguna
    // llamada a LVGL cruce hilos.
    char sbuf[OTA_STATUS_MAX];
    int  spct = -1;
    bool sactive = false;
    OTA_STATUS_LOCK();
    if (ota_status_valid) {
        for (size_t i = 0; i < sizeof(sbuf); i++) sbuf[i] = ota_status_text[i];
        spct = ota_status_pct;
        sactive = (int32_t)(now - ota_status_until) < 0;
        if (!sactive) ota_status_valid = false;   // caducó → dejar de repetirla
    }
    OTA_STATUS_UNLOCK();

    if (sactive || ota_is_active() || ota_pull_is_active()) {
        static char obuf[64];
        if (sactive) {
            if (spct >= 0)
                snprintf(obuf, sizeof(obuf), "%s %s\xE2\x80\xA6 %d%%",
                         spinner_frames[anim_spinner_idx], sbuf, spct);
            else
                snprintf(obuf, sizeof(obuf), "%s %s\xE2\x80\xA6",
                         spinner_frames[anim_spinner_idx], sbuf);
        } else {
            snprintf(obuf, sizeof(obuf), "%s OTA\xE2\x80\xA6",
                     spinner_frames[anim_spinner_idx]);
        }
        lv_label_set_text(lbl_anim, obuf);
        return;
    }

    const char* text;
    if (view_state == 3) {
        text = "Emparejando";     // código de emparejamiento del backend en pantalla
    } else if (!s_ble_connected && view_state != 2) {
        text = "Esperando";       // publicitando / esperando la conexión de un host
    } else if (view_state == 1) {  // reposo: alternar para que se lea vivo y sin datos a la vez
        text = (anim_msg_idx & 1) ? "Sin datos" : "Escuchando";
    } else if (s_ble_connected && now - connected_at_ms < 5000) {
        text = "Conectado";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // Todos los estados comparten el estilo fantasioso: "<glifo> <palabra con mayúscula inicial>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

// Traspaso no bloqueante del worker del pull de OTA a la interfaz. Solo copia el
// texto + el porcentaje y estampa una fecha límite bajo una sección crítica
// pequeña; el lv_label_set_text de verdad lo hace ui_tick_anim() en la tarea del
// loop, así que se puede llamar desde cualquier tarea y nunca toca LVGL.
// pct < 0 = sin porcentaje.
void ui_ota_status(const char* text, int pct) {
    uint32_t until = lv_tick_get() + OTA_STATUS_MS;
    OTA_STATUS_LOCK();
    size_t i = 0;
    if (text) {
        for (; text[i] && i < sizeof(ota_status_text) - 1; i++) {
            ota_status_text[i] = text[i];
        }
    }
    ota_status_text[i] = '\0';
    ota_status_pct   = pct;
    ota_status_until = until;
    ota_status_valid = true;
    OTA_STATUS_UNLOCK();
}

// Las dos pantallas de splash no tienen palabras (Clawd, o la escena de OpenCode
// en el mismo lienzo compartido), así que el indicador de batería se aparta para
// ellas; las tres pantallas de datos lo conservan. La mascota de la esquina y el
// logo quieto son solo de la pantalla de uso de Claude: las de OpenCode llevan su
// propia marca y el portfolio su propio encabezado.
static bool screen_is_splash(screen_t s) {
    return s == SCREEN_SPLASH || s == SCREEN_OC_SPLASH;
}
static bool screen_is_claude_usage(screen_t s) {
    return s == SCREEN_USAGE;
}

static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (screen_is_splash(current_screen)) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// ---- Command Center (design/command-center/SPEC.md §3) ----
// Overlay fuera del ciclo: no entra a screen_t, no llama show_page_dots() y no
// cambia la navegación. Un solo handler a nivel de ciclo (no uno por pantalla):
// en PRESSED guarda la y, en GESTURE abre (borde superior hacia abajo) o cierra
// (hacia arriba en cualquier y). Los gestos suben hasta la pantalla por el
// gesture_bubble que LVGL activa por defecto en cada hijo, así que basta con
// escuchar en lv_screen_active().
#define SYS_EDGE_Y 60   // el gesto de apertura empieza en el borde superior
static int16_t sys_press_y = -1;   // y del último PRESSED, -1 = ninguno aún

static void sys_gesture_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    if (code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        sys_press_y = p.y;
    } else if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (!sys_is_open() && dir == LV_DIR_BOTTOM &&
            sys_press_y >= 0 && sys_press_y < SYS_EDGE_Y) {
            sys_show();   // abre sobre la pantalla activa, la de abajo intacta
        } else if (sys_is_open() && dir == LV_DIR_TOP) {
            sys_hide();
        }
    } else if (code == LV_EVENT_CLICKED) {
        // El tap sobre el overlay solo cierra, sin burbuja a global_click_cb.
        if (sys_is_open()) sys_hide();
    }
}

// SPEC.md §6: un toque en cualquier lugar avanza un paso en el ciclo.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    // Con el overlay abierto el tap solo cierra, no navega (§3.2).
    if (sys_is_open()) { sys_hide(); return; }
    ui_next_screen();
}

// El orden del ciclo. Las dos pantallas de OpenCode se unen recién cuando llega
// un payload (oc_has_data()), y el portfolio cuando llega uno de "k":"pf"
// (pf_has_data()); es justo lo que SPEC.md §6 quiere decir con "si esos datos
// nunca llegan, el ciclo se queda en splash de Clawd ↔ uso de Claude".
static screen_t cycle_at(int i) {
    switch (i) {
    case 0:  return SCREEN_SPLASH;
    case 1:  return SCREEN_USAGE;
    case 2:  return SCREEN_OC_SPLASH;
    case 3:  return SCREEN_OC_USAGE;
    default: return SCREEN_PORTFOLIO;
    }
}

static int cycle_len(void) {
    int n = oc_has_data() ? 4 : 2;
    if (pf_has_data()) n++;
    return n;
}

static int cycle_index(screen_t s) {
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (cycle_at(i) == s) return i;
    }
    return -1;
}

static void ui_step(int dir) {
    int n = cycle_len();
    int i = cycle_index(current_screen);
    // Fuera del ciclo (solo alcanzable si un tipo de payload desapareciera, lo que
    // no puede pasar: las banderas son pegajosas) o n==0: caer al vecino de la
    // pantalla de uso de Claude.
    if (i < 0) i = (n > 0) ? 0 : 1;
    i = ((i + dir) % n + n) % n;
    ui_show_screen(cycle_at(i));
}

void ui_next_screen(void) {
    ui_step(+1);
}

void ui_prev_screen(void) {
    ui_step(-1);
}

// Muestra la franja de puntos durante 1,5 s. Se llama en cada cambio de
// pantalla, así que la franja también crece de 2 a 4 a 5 puntos la primera vez
// que aterriza cada tipo de payload.
static void show_page_dots(void) {
    int n = cycle_len();
    int i = cycle_index(current_screen);
    if (i < 0 || i >= n) i = 0;
    set_page_dots(n, i);
    lv_obj_clear_flag(dots_root, LV_OBJ_FLAG_HIDDEN);

    dots_until_ms = lv_tick_get() + PAGE_DOTS_MS;
}

void ui_show_screen(screen_t screen) {
    // Soltar los recursos de la pantalla actual antes de cambiar: las pantallas de
    // OpenCode tienen estado propio que hay que liberar a mano.
    if (current_screen == SCREEN_OC_USAGE)  oc_usage_hide();
    if (current_screen == SCREEN_PORTFOLIO) pf_usage_hide();
    if (current_screen == SCREEN_OC_SPLASH) oc_splash_stop();

    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:
        // Clawd vuelve a ser dueño del lienzo y después elige una animación según
        // el ritmo.
        oc_splash_stop();
        splash_show();
        break;
    case SCREEN_USAGE:
        lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
        // La etiqueta del reloj hace de título de esta pantalla; la pantalla de
        // OpenCode trae su propio encabezado, así que no debe quedar detrás.
        lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_OC_SPLASH:
        // El lienzo compartido de Clawd, otro dueño: mostrarlo primero (lo que
        // se salta la elección de Clawd, ver splash.h) y después entregárselo.
        splash_show();
        oc_splash_start();
        break;
    case SCREEN_OC_USAGE:
        // Primera visita: construye la pantalla (perezosa, ver ui_init) y la muestra.
        if (!oc_usage_get_root()) {
            oc_usage_init(lv_screen_active());
            if (oc_usage_get_root()) {
                lv_obj_add_event_cb(oc_usage_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
            }
        }
        oc_usage_show();
        break;
    case SCREEN_PORTFOLIO:
        // La primera visita construye la pantalla; las siguientes solo la muestran.
        // Hasta entonces la raíz es NULL y pf_usage_get_root() devuelve NULL, que es
        // como el ciclo mantiene esta pantalla afuera cuando nunca llegó un payload.
        if (!pf_usage_get_root()) {
            pf_usage_init(lv_screen_active());
            if (pf_usage_get_root()) {
                lv_obj_add_event_cb(pf_usage_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
            }
        }
        pf_usage_show();
        break;
    default:
        break;
    }

    if (screen == SCREEN_OC_USAGE && lbl_title) {
        lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
    }

    splash_mascot_set_visible(screen_is_claude_usage(screen));
    if (logo_img) {
        if (screen_is_claude_usage(screen)) lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                                 lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    current_screen = screen;
    apply_battery_visibility();
    show_page_dots();
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

// ---- Tubería de OpenCode (SPEC.md §5, §8) ----
//
// Un payload alimenta a tres consumidores: la pantalla de uso de OpenCode, el
// ánimo del splash de OpenCode y la animación de ensamblado que se reproduce
// cuando una ventana de uso se recargó. La prueba de caída de porcentaje compara
// contra el payload anterior, así que el reinicio de una ventana se detecta en el
// latido que lo reporta y no en el momento en que la cuenta llega a cero (que el
// firmware nunca ve).
void ui_update_opencode(const OcData* data) {
    if (!data) return;

    oc_usage_update(data);

    static bool have_prev = false;
    static int  prev_p5 = 0, prev_pw = 0;
    // Una caída de 5 puntos en una hora o en una semana es una ventana que se
    // acaba de reiniciar: repetir una vez el ensamblado de la marca y después
    // devolverle la escena al ánimo.
    if (have_prev &&
        (prev_p5 - data->p5 >= 5 || prev_pw - data->pw >= 5)) {
        oc_splash_play_assemble();
    }
    prev_p5 = data->p5;
    prev_pw = data->pw;
    have_prev = true;

    // Ánimo, en el orden del SPEC.md §5: el límite gana sobre todo, después lo
    // cerca que está la ventana y por último cuántas sesiones están corriendo.
    int peak = (data->p5 > data->pw) ? data->p5 : data->pw;
    oc_mood_t mood;
    if (data->limited)        mood = OC_MOOD_LIMITED;
    else if (peak >= 75)      mood = OC_MOOD_NEAR;
    else if (data->a >= 2)    mood = OC_MOOD_BUSY;
    else if (data->a >= 1)    mood = OC_MOOD_ACTIVE;
    else                      mood = OC_MOOD_IDLE;
    oc_splash_set_mood(mood);
}

// ---- Tubería del portfolio (especificación del portfolio §6) ----
//
// Un payload, un consumidor. A propósito es toda la historia: el daemon nunca se
// entera de qué pantalla está visible, y el modo privado es un interruptor de
// visibilidad del lado del firmware sobre exactamente los mismos números
// (especificación de privacidad §6).
void ui_update_portfolio(const PfData* data) {
    if (!data) return;
    pf_usage_update(data);
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // emparejando / reposo / uso: se elige por conexión + frescura de datos.
    update_view_state();
    // La pantalla de OpenCode atenúa sus paneles cuando se cae el enlace y lo dice
    // en su línea de estado: misma noción de "conectado" que la pantalla de Claude
    // de arriba, y el mismo tratamiento en la del portfolio.
    oc_usage_set_ble(s_ble_connected);
    pf_usage_set_ble(s_ble_connected);
}

void ui_update_battery(int percent, bool charging) {
    if (!battery_img) return;
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
