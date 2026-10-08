#include "ui_portfolio.h"
#include "pf_privacy.h"
#include <lvgl.h>
#include <string.h>

#include "hal/board_caps.h"

// Styrene B Regular (SIL OFL 1.1), los mismos cortes compilados que usan las
// pantallas de Claude. Los cortes cubren ASCII (0x20-0x7E), los glifos del
// español (á é í ó ú ü ñ Á É Í Ó Ú Ü Ñ ¡ ¿) y también · y …. Ninguna cadena de
// esta pantalla necesita ninguno de esos glifos extra, así que dos
// consecuencias del ASCII siguen dándole forma (spec §6.2): el separador de
// cláusulas es la coma, y un símbolo que no cabe se corta a N caracteres más un
// '.', nunca una elipsis. Un glifo que falte tampoco es un hueco en esta
// compilación: LV_USE_FONT_PLACEHOLDER está activo, así que se dibujaría como un
// rectángulo.
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);

// Fichas de diseño del portafolio (spec de diseño §4). Son locales a este
// archivo a propósito: las pantallas de Claude se quedan con theme.h. Dos
// tarjetas, sin contorno de 1 px y sin relleno de chip: `border` y `element` no
// existen, las tarjetas son superficies redondeadas. Desviación deliberada de la
// spec de OpenCode: `primary` solo se usa en palabras que empiezan por "!" y
// siempre sobre el fondo negro, nunca dentro de una tarjeta — en esta pantalla
// el rojo y el verde significan "el dinero subió / bajó" y nada más, y un número
// y un aviso nunca comparten superficie.
#define PF_SURFACE   lv_color_hex(0x1a1a1a)   // las dos tarjetas
#define PF_TEXT      lv_color_hex(0xeeeeee)   // valor de mercado, símbolos, punto de "En vivo"
#define PF_MUTED     lv_color_hex(0x8c8c8c)   // etiquetas, precio de la acción, estado de la sesión
#define PF_PRIMARY   lv_color_hex(0xfab283)   // solo avisos con "!", sobre negro
#define PF_ERROR     lv_color_hex(0xe06c75)   // dinero negativo
#define PF_SUCCESS   lv_color_hex(0x2ee88a)   // dinero positivo

// La frescura es la del propio firmware: el latido del daemon no reenvía `pf` a
// propósito, y es eso lo que permite que "Sin actualizar" llegue a aparecer.
// Solo cambia el texto de estado cuando s == "l", y nunca atenúa nada.
#define PF_FRESH_MS  300000u
#define PF_ROWS      4
// El símbolo más corto que se le permite mostrar a una fila (spec §2.3.1). Su
// ancho se mide con la fuente de fila al maquetar, nunca se codifica a mano:
// 54 px en Styrene 16. Es el suelo contra el que el guardián compara el
// presupuesto de una fila antes de empezar a soltar columnas.
#define PF_MIN_SYM   "WWW."

// ---- Medida del texto --------------------------------------------------------
// El ancho, en px, que va a dibujar LVGL. La spec pide lv_text_get_width(), que
// es la llamada correcta: suma los avances *reales* por glifo, así que los ±1-2
// px que una tabla de anchos deja fuera (Styrene redondea cada glifo por su
// cuenta, lv_font_fmt_txt.c: `(adv_w + 8) >> 4`) desaparecen — lo que se mide es
// lo que se dibuja. En LVGL 9 esa función recibe un lv_text_attributes_t
// privado, así que aquí se hace la misma suma con la API pública por glifo que
// ella llama una vez por letra. Todas las cadenas de esta pantalla son ASCII por
// regla, así que recorrer bytes es recorrer caracteres.
static int32_t text_w(const char *s, const lv_font_t *f) {
    int32_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        w += lv_font_get_glyph_width(f, *p, p[1]);   // p[1] alimenta el kerning
    return w;
}

// ---- Maquetación -------------------------------------------------------------
// La tabla `LT` de la spec de diseño §2.3, tal cual: un bloque por placa, todo
// en px, con `top` medido desde el borde superior de la tarjeta. Cada clave
// `*Ri` es un margen desde el borde derecho de la tarjeta (CSS `right: Npx`) y
// nunca una distancia desde la izquierda, así que una tarjeta más ancha o más
// estrecha que la de la tabla nunca puede sacar un número de ella (§2.2). El
// ancho de tarjeta es siempre `scr_w - 2 * card_x`, y es eso lo que deja que la
// AMOLED-2.06 de 410x502 caiga en `compacto` en el breakpoint de abajo y se
// maquete igual desde la derecha.
struct RowFields {
    int16_t sym_x, gap;                 // origen del símbolo y el aire mínimo entre columnas
    int16_t price_ri, pct_ri, contrib_ri;   // márgenes desde el borde derecho de la tarjeta; -1 = sin columna
};

struct PfLayout {
    int16_t scr_w, scr_h;

    int16_t header_x, header_top;
    const lv_font_t *header_font;

    int16_t card_x, card_w, pad;
    int16_t c1_y, c1_h, c1_radius;
    int16_t c2_y, c2_h, c2_radius;

    // tarjeta 1
    bool     has_label;                 // la placa chica no tiene etiqueta en la tarjeta 1
    int16_t  label_top;  const lv_font_t *label_font;
    int16_t  unit_top, unit_ri;  const lv_font_t *unit_font;   // "COP"
    int16_t  hero_top;  const lv_font_t *hero_font;            // el total, o el porcentaje del día
    bool     has_cols;                  // dos columnas apiladas; false = la línea de día de la placa chica
    int16_t  col_label_top, col_val_top;
    const lv_font_t *col_label_font, *col_val_font;
    int16_t  col1_x, col2_x;            // borde izquierdo de cada columna
    int16_t  day_top, day_dc_x, day_pct_ri;   // la línea única de día de la placa chica

    // tarjeta 2
    int16_t  title_top;  const lv_font_t *title_font;  int16_t count_ri;
    int16_t  rows_top, rows_pitch, rows_gap;
    const lv_font_t *rows_font;         // Styrene 16 en todas las placas, nunca 12
    RowFields f, f_priv;
    int16_t  norows_top;  const lv_font_t *norows_font;

    // tarjeta 1 en un estado vacío
    int16_t  e_h, e1_top, e2_top, e3_top;
    const lv_font_t *e1_font, *e2_font;
    bool     short_paths;               // la placa chica acorta la ruta de config

    // línea de estado: en la pantalla, sobre negro
    int16_t  st_x, st_top, st_dot, st_dot_y, st_text_x;
    const lv_font_t *st_font;

    // deltas del modo privado (spec de privacidad §7)
    int16_t pv_c1_h, pv_c2_y;
};
static PfLayout L = {};

// Tres bloques, los mismos tres breakpoints que usa ui.cpp. El grande necesita
// las dos dimensiones: la AMOLED-2.06 mide 502 px de alto, así que solo
// `H >= 460` le daría la maquetación de 480 sobre una tarjeta de 370 px, y los
// números definidos desde la izquierda se salían de ella.
static void compute_layout(const BoardCaps &c) {
    L.scr_w = c.width;
    L.scr_h = c.height;

    if (c.width >= 460 && c.height >= 460) {
        // ---- grande: 480x480 (AMOLED-2.16, LCD-4, sim) ----
        L.header_x = 20; L.header_top = 36;  L.header_font = &font_styrene_16;
        L.card_x = 20;  L.pad = 20;
        L.c1_y = 72;  L.c1_h = 172; L.c1_radius = 12;      // 72..244
        L.c2_y = 256; L.c2_h = 180; L.c2_radius = 12;      // 256..436
        L.has_label = true;
        L.label_top = 18; L.label_font = &font_styrene_16;
        L.unit_top = 18; L.unit_ri = 20; L.unit_font = &font_styrene_16;
        L.hero_top = 40; L.hero_font = &font_styrene_48;
        L.has_cols = true;
        L.col_label_top = 104; L.col_val_top = 124;
        L.col_label_font = &font_styrene_16; L.col_val_font = &font_styrene_28;
        L.col1_x = 20; L.col2_x = 240;
        L.day_top = 0; L.day_dc_x = 0; L.day_pct_ri = 0;
        L.title_top = 18; L.title_font = &font_styrene_16; L.count_ri = 20;
        L.rows_top = 48; L.rows_pitch = 28; L.rows_gap = 10;
        L.rows_font = &font_styrene_16;
        L.f = { 20, 16, 199, 118,  20 };
        L.f_priv = { 20, 16, 101,  20, -1 };
        L.norows_top = 48; L.norows_font = &font_styrene_16;
        L.e_h = 114; L.e1_top = 20; L.e2_top = 58; L.e3_top = 80;
        L.e1_font = &font_styrene_24; L.e2_font = &font_styrene_14;
        L.short_paths = false;
        L.st_x = 20; L.st_top = 448; L.st_dot = 8; L.st_dot_y = 452; L.st_text_x = 36;
        L.st_font = &font_styrene_16;
        L.pv_c1_h = 109; L.pv_c2_y = 193;
    } else if (c.height >= 300) {
        // ---- compacto: 368x448 (AMOLED-1.8) y 410x502 (AMOLED-2.06) ----
        L.header_x = 20; L.header_top = 32;  L.header_font = &font_styrene_14;
        L.card_x = 20;  L.pad = 16;
        L.c1_y = 64;  L.c1_h = 150; L.c1_radius = 12;      // 64..214
        L.c2_y = 224; L.c2_h = 166; L.c2_radius = 12;      // 224..390
        L.has_label = true;
        L.label_top = 14; L.label_font = &font_styrene_14;
        L.unit_top = 14; L.unit_ri = 16; L.unit_font = &font_styrene_14;
        L.hero_top = 32; L.hero_font = &font_styrene_48;
        L.has_cols = true;
        L.col_label_top = 94; L.col_val_top = 111;
        L.col_label_font = &font_styrene_14; L.col_val_font = &font_styrene_24;
        L.col1_x = 16; L.col2_x = 164;
        L.day_top = 0; L.day_dc_x = 0; L.day_pct_ri = 0;
        L.title_top = 14; L.title_font = &font_styrene_14; L.count_ri = 16;
        L.rows_top = 40; L.rows_pitch = 28; L.rows_gap = 8;
        L.rows_font = &font_styrene_16;
        // Sin contribución: no cabe al lado del precio en 328 px, y el precio es
        // la columna que el usuario confronta con el bróker (§2.1).
        L.f = { 16, 12, 106, 16, -1 };
        L.f_priv = { 16, 12, 106, 16, -1 };
        L.norows_top = 40; L.norows_font = &font_styrene_14;
        L.e_h = 94; L.e1_top = 16; L.e2_top = 46; L.e3_top = 64;
        L.e1_font = &font_styrene_20; L.e2_font = &font_styrene_12;
        L.short_paths = false;
        L.st_x = 20; L.st_top = 404; L.st_dot = 8; L.st_dot_y = 407; L.st_text_x = 34;
        L.st_font = &font_styrene_14;
        L.pv_c1_h = 98; L.pv_c2_y = 172;
    } else {
        // ---- chico: 240x240 (breakpoint compacto, hoy sin uso) ----
        L.header_x = 8; L.header_top = 10;  L.header_font = &font_styrene_12;
        L.card_x = 8;  L.pad = 8;
        L.c1_y = 30;  L.c1_h = 63;  L.c1_radius = 8;        // 30..93
        L.c2_y = 99;  L.c2_h = 109; L.c2_radius = 8;        // 99..208
        L.has_label = false;           // no hay sitio: el número es la etiqueta
        L.label_top = 0; L.label_font = &font_styrene_12;
        L.unit_top = 19; L.unit_ri = 8; L.unit_font = &font_styrene_12;
        L.hero_top = 6; L.hero_font = &font_styrene_28;
        L.has_cols = false;            // la variación del día es una sola línea de 16 px
        L.col_label_top = 0; L.col_val_top = 0;
        L.col_label_font = &font_styrene_12; L.col_val_font = &font_styrene_16;
        L.col1_x = 0; L.col2_x = 0;
        L.day_top = 40; L.day_dc_x = 8; L.day_pct_ri = 8;
        L.title_top = 6; L.title_font = &font_styrene_12; L.count_ri = 8;
        L.rows_top = 24; L.rows_pitch = 19; L.rows_gap = 4;
        L.rows_font = &font_styrene_16;
        // Solo símbolo y porcentaje. El precio necesitaría 71 px que aquí no
        // están (§2.1), y una columna que aparece y desaparece con los precios
        // del día no es una columna.
        L.f = { 8, 10, -1, 8, -1 };
        L.f_priv = { 8, 10, -1, 8, -1 };
        L.norows_top = 24; L.norows_font = &font_styrene_12;
        L.e_h = 64; L.e1_top = 6; L.e2_top = 28; L.e3_top = 44;
        L.e1_font = &font_styrene_16; L.e2_font = &font_styrene_12;
        L.short_paths = true;
        L.st_x = 8; L.st_top = 214; L.st_dot = 6; L.st_dot_y = 218; L.st_text_x = 20;
        L.st_font = &font_styrene_12;
        L.pv_c1_h = 42; L.pv_c2_y = 78;
    }

    // Las tarjetas ocupan la pantalla menos los dos márgenes, así que una placa
    // en cualquier breakpoint con un panel más estrecho que el de la tabla
    // mantiene todos los márgenes internos dentro de ella.
    L.card_w = L.scr_w - 2 * L.card_x;
}

// ---- Widgets ----------------------------------------------------------------
// Presupuesto de objetos: el pozo compartido de LVGL es de 96 KB en todas las
// placas, y las cuatro pantallas previas ya ocupan ~48 KB de él — las placas C6
// no tienen PSRAM donde ampliarlo. Así que el juego de widgets de aquí se
// mantiene tan pequeño como la spec permite: el valor de mercado y el héroe del
// modo privado son la misma etiqueta, las dos tarjetas se atenúan directamente
// en vez de mediante un grupo extra a pantalla completa, el separador de 1 px no
// existe (el hueco entre los grupos es el aire), y las celdas de fila son los
// únicos objetos por fila. Una columna solo recibe etiquetas en las placas cuya
// tabla la tiene, así que la placa chica nunca paga por un precio que no va a
// dibujar.
static lv_obj_t *root;          // contenedor de la pantalla
static lv_obj_t *header_lbl;
static lv_obj_t *card1, *card2;
static lv_obj_t *lbl_label, *lbl_unit, *lbl_hero;
static lv_obj_t *lbl_col1, *lbl_col2, *lbl_dc, *lbl_dc_pct;
static lv_obj_t *lbl_e[3];
static lv_obj_t *lbl_title, *lbl_count, *lbl_norows;
static lv_obj_t *row_sym[PF_ROWS], *row_price[PF_ROWS];
static lv_obj_t *row_pct[PF_ROWS], *row_contrib[PF_ROWS];
static lv_obj_t *status_dot, *status_lbl;

// ---- State ------------------------------------------------------------------
static PfData   cur;
static bool     have_data = false;   // llegó un payload válido desde el arranque
static uint32_t data_ms = 0;
static bool     ble_on = false;
static bool     dimmed = false;
static int      mode_applied = -1;    // -1 desconocido / 0 normal / 1 privado
static int16_t  card1_h = -1;         // altura de la tarjeta 1 hoy en LVGL
static bool     sq_shown = false;     // el punto fijo de "En vivo"
static int16_t   min_sym_w = 0;       // W("WWW.") en la fuente de fila
// Las dos empiezan "sin definir" (el negro nunca es un color de estado, -1 es
// ninguna x) para que el primer dibujo siempre las envíe; después de eso solo
// los cambios reales llegan a LVGL.
static lv_color_t status_color;
static int16_t   status_x = -1;
static char      status_text[48] = "";

// El texto de una fila, medido. Es estática para que las dos pasadas de
// maquetación de abajo puedan compartirla sin una trama de 250 bytes en un
// redibujado que ya corre en la pila de la tarea de LVGL. Nada de lo que hay
// aquí dentro es estado que tenga que sobrevivir a un redibujado.
struct RowGeom {
    char    sym[PF_SYM_MAX + 2];
    char    price[16];
    char    pct[12];
    char    contrib[16];
    int16_t w_price, w_pct, w_contrib;
    int16_t r_contrib, r_pct, r_price;  // bordes derechos dentro de la tarjeta
    int16_t top;
};
static RowGeom rowg[PF_ROWS];

static void redraw(void);

// ---- Formato (spec de portfolio §7) -----------------------------------------
//
// Coma para los miles y punto para los decimales — el formato del propio
// bróker, para que la pantalla le coincida cifra a cifra. '+' / '-' ASCII / nada
// para el cero. El redondeo ocurre siempre *antes* de elegir la unidad, así que
// 999,600 nunca puede imprimirse como "1000k". Sin float, sin U+2212 y sin
// paréntesis de contabilidad.

static const char *pf_sign(long n) {
    return n > 0 ? "+" : n < 0 ? "-" : "";
}

// Valor absoluto con una coma cada tres dígitos (el signo es del que llama).
static void fmt_int(char *buf, size_t len, long v) {
    char tmp[16];
    int n = 0;
    unsigned long a = (unsigned long)(v < 0 ? -v : v);
    do { tmp[n++] = (char)('0' + (a % 10)); a /= 10; } while (a);
    size_t o = 0;
    for (int i = n - 1; i >= 0 && o + 1 < len; i--) {
        // Los grupos se recorren de derecha a izquierda, así que una coma abre
        // grupo nuevo allí donde ya hay un grupo completo de tres: `i` es la
        // cuenta de dígitos escritos hasta ahora, y el dígito de más a la
        // izquierda (i == n-1) nunca la lleva.
        // 66260 -> "66,260", 122960 -> "122,960", 375 -> "375".
        if (i < n - 1 && ((i + 1) % 3) == 0 && o + 1 < len) buf[o++] = ',';
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
}

// Valor de mercado: es un nivel, así que nunca lleva signo ni color.
//   "845,300" · "132.9M" · "1.33B"
static const char *fmt_market_value(long mv) {
    static char buf[16];
    long a = mv < 0 ? -mv : mv;
    if (a < 1000000L) {
        fmt_int(buf, sizeof buf, mv);
    } else if (a < 999950000L) {
        long t = (a + 50000L) / 100000L;          // tenths of a million
        snprintf(buf, sizeof buf, "%ld.%ldM", t / 10, t % 10);
    } else {
        long h = (a + 5000000L) / 10000000L;       // hundredths of a billion
        snprintf(buf, sizeof buf, "%ld.%02ldB", h / 100, h % 100);
    }
    return buf;
}

// COP con signo: "< 1M completo con comas, si no X.XXM" — la variación del día
// y la contribución de la fila.   "-34,880" · "+812,400" · "-1.33M"
static const char *fmt_cop_full(long n) {
    static char buf[16];
    char ib[16];
    long a = n < 0 ? -n : n;
    if (a < 1000000L) {
        fmt_int(ib, sizeof ib, n);
        snprintf(buf, sizeof buf, "%s%s", pf_sign(n), ib);
    } else {
        long h = (a + 5000L) / 10000L;            // hundredths of a million
        snprintf(buf, sizeof buf, "%s%ld.%02ldM", pf_sign(n), h / 100, h % 100);
    }
    return buf;
}

// Porcentaje de fila: signo + 2 decimales + "%" (máx. 7).   "+1.28%" · "-10.25%"
static const char *fmt_pct_row(int cents) {
    static char buf[12];
    long a = cents < 0 ? -(long)cents : cents;
    snprintf(buf, sizeof buf, "%s%ld.%02ld%%", pf_sign(cents), a / 100, a % 100);
    return buf;
}

// Porcentaje del día: el signo sale de `dc`, nunca de `dp`, así que un día cuyo
// porcentaje redondea a cero muestra igual el signo real ("-0.00%").
//   "-0.03%" · "+1.10%" · "0.00%" cuando dc == 0
static const char *fmt_pct_day(int dp, long dc) {
    static char buf[12];
    long a = dp < 0 ? -(long)dp : dp;
    if (dc == 0) {
        snprintf(buf, sizeof buf, "0.00%%");
    } else {
        snprintf(buf, sizeof buf, "%s%ld.%02ld%%", pf_sign(dc), a / 100, a % 100);
    }
    return buf;
}

// Precio por acción: completo, nunca abreviado — el precio se confronta con el
// bróker y una "k" borraría justo el tick que cambió. Es además la única cifra
// en pesos que sobrevive al modo privado: es una cotización pública, idéntica
// para quien tenga una acción o cien mil (spec de privacidad §7.1).
static const char *fmt_price(long p) {
    static char buf[16];
    fmt_int(buf, sizeof buf, p);
    return buf;
}

// Símbolo sin su sufijo .CL, cortado a las primeras N caracteres que caben en
// el presupuesto propio de la fila más un '.' (spec §6.2.3). El corte es por
// ancho medido, no por cuenta de caracteres, porque Styrene es proporcional:
// 'I' y 'W' no miden lo mismo. El punto es la marca de abreviación del español,
// existe en todos los cortes, y es lo que evita que "PFGRUPOAR." se lea como un
// ticker que existe. Nunca es una elipsis: estas fuentes no tienen U+2026 y un
// glifo que falte se dibujaría como un rectángulo. Tres letras es el suelo — por
// debajo la fila deja de identificar la acción — así que un nombre que no cabe
// no se dibuja en absoluto, y es el guardián del que llama (§2.3.1) el que le
// hace sitio.
static const char *fmt_sym(const char *sym, int16_t budget, char *out, size_t len) {
    int n = (int)strlen(sym);
    if (n == 0) { out[0] = '\0'; return out; }
    if (text_w(sym, L.rows_font) <= budget) { strlcpy(out, sym, len); return out; }
    // No cabe, y un nombre cortado por debajo de tres letras dejaría de
    // identificar la acción, así que no se dibuja en absoluto: es el guardián
    // del que llama el que le hace sitio soltando una columna.
    if (n <= 3) { out[0] = '\0'; return out; }
    for (n = n - 1; ; n--) {
        snprintf(out, len, "%.*s.", n, sym);
        if (n <= 3 || text_w(out, L.rows_font) <= budget) return out;
    }
}

// t = "MMDDhhmm", hora de Colombia del último precio: "10:42"
static const char *fmt_clock(const char *t) {
    static char buf[8];
    if (!t || strlen(t) < 8) { strlcpy(buf, "--:--", sizeof buf); return buf; }
    snprintf(buf, sizeof buf, "%.2s:%.2s", t + 4, t + 6);
    return buf;
}

// La misma marca de tiempo como cierre de sesión: "29 sep" (día sin cero inicial)
static const char *fmt_session_date(const char *t) {
    static const char *const months[] = { "ene", "feb", "mar", "abr", "may", "jun",
                                          "jul", "ago", "sep", "oct", "nov", "dic" };
    static char buf[10];
    if (!t || strlen(t) < 8) { strlcpy(buf, "-- ---", sizeof buf); return buf; }
    int mm = (t[0] - '0') * 10 + (t[1] - '0');
    int dd = (t[2] - '0') * 10 + (t[3] - '0');
    if (mm < 1 || mm > 12) { strlcpy(buf, "-- ---", sizeof buf); return buf; }
    snprintf(buf, sizeof buf, "%d %s", dd, months[mm - 1]);
    return buf;
}

// Verde por encima, rojo por debajo, blanco sin más en el cero — y lo único que
// toca son números con signo.
static lv_color_t money_color(long n) {
    return n > 0 ? PF_SUCCESS : n < 0 ? PF_ERROR : PF_TEXT;
}

// ---- La fila: anclada a la derecha, medida, con el excedente para el símbolo
// (spec §2.3.1) ---
//
// Se compone desde el borde derecho hacia dentro, una vez por fila, con los
// anchos de las cadenas que realmente se van a dibujar:
//
//   cR  = cardW - contribRi            (siempre contra el relleno: es la
//   cL  = cR - W(contrib)              columna de más a la derecha, nada puede
//                                       moverla)
//   pR  = min(cardW - pctRi,  cL - gap)
//   pL  = pR - W(pct)
//   prR = min(cardW - priceRi, pL - gap)
//   prL = prR - W(price)
//   symBudget = prL - gap - symX       (lo que sobre es del símbolo)
//
// Dos propiedades salen del `min` y son todo el sentido del cambio: los márgenes
// por defecto mantienen las columnas alineadas de fila a fila, y un valor más
// ancho de lo previsto solo empuja las columnas que tiene a su izquierda, nunca
// las pisa. El sobrante va al símbolo, que se corta hasta caber. El margen de
// una fila es por tanto "cuántos px pueden crecer todavía los números", no
// "cuánto sobró de una constante".
static int16_t row_layout(const RowGeom *g, const RowFields *f, bool with_c, bool with_pr,
                          int16_t *r_c, int16_t *r_p, int16_t *r_pr) {
    int32_t left = INT32_MAX;                       // borde izquierdo de la columna de la derecha
    if (with_c) {
        int32_t r = (int32_t)L.card_w - f->contrib_ri;
        *r_c = (int16_t)r;
        left = r - g->w_contrib;
    }
    int32_t p = (int32_t)L.card_w - f->pct_ri;
    if (p > left - f->gap) p = left - f->gap;       // INT32_MAX deja el valor por defecto intacto
    *r_p = (int16_t)p;
    int32_t p_left = p - g->w_pct;
    left = p_left;
    if (with_pr) {
        int32_t r = (int32_t)L.card_w - f->price_ri;
        if (r > p_left - f->gap) r = p_left - f->gap;
        *r_pr = (int16_t)r;
        left = r - g->w_price;
    }
    return (int16_t)(left - f->gap - f->sym_x);
}

// El presupuesto de símbolo más ajustado entre las filas en pantalla, que es
// contra lo que compara el guardián. La pasada 1 ya formateó y midió cada fila,
// así que esta es la misma aritmética que hace la colocación de abajo, sin
// tocarla.
static int16_t tightest_budget(const RowFields *f, int n, bool with_c, bool with_pr) {
    int16_t tightest = INT16_MAX;
    for (int i = 0; i < n; i++) {
        int16_t r_c, r_p, r_pr;
        int16_t b = row_layout(&rowg[i], f, with_c, with_pr, &r_c, &r_p, &r_pr);
        if (b < tightest) tightest = b;
    }
    return tightest;
}

// ---- Builders ---------------------------------------------------------------

// Grupo transparente, sin borde y permeable al tacto. EVENT_BUBBLE en todas
// partes para que un toque en cualquier sitio llegue a la raíz de la pantalla,
// que es donde escucha ui.cpp.
static lv_obj_t *make_group(lv_obj_t *parent) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_size(g, L.scr_w, L.scr_h);
    lv_obj_set_pos(g, 0, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, LV_OBJ_FLAG_EVENT_BUBBLE);
    return g;
}

// Una tarjeta: una superficie oscura redondeada y sin contorno. La agrupación la
// hace el relleno, no una línea, así que radio 12 / 12 / 8 y border_width 0
// (spec §4). Los hijos se posicionan a mano.
static lv_obj_t *make_card(lv_obj_t *parent, int16_t y, int16_t h, int16_t radius) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, L.card_x, y);
    lv_obj_set_size(p, L.card_w, h);
    lv_obj_set_style_bg_color(p, PF_SURFACE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
    return p;
}

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
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);   // una sola línea, como nowrap
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    return l;
}

// Coloca una etiqueta con su *borde derecho* a `inset` px del borde derecho de
// la tarjeta (CSS `right: Npx`). lv_obj_align(TOP_RIGHT, x_ofs) desplaza el objeto
// hacia fuera x_ofs, así que el margen interno tiene que negarse.
static void place_right(lv_obj_t *l, int16_t inset, int16_t top) {
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -inset, top);
}

// Igual, desde un borde derecho que calculó el algoritmo de fila: `r` se mide
// desde el borde izquierdo de la tarjeta, así que el margen interno es lo que
// quede entre él y el borde derecho de la tarjeta. Es la única forma de colocar
// una columna: no hay composición de izquierda a derecha ni distancia desde la
// izquierda en ningún sitio (spec §2.2).
static void place_at(lv_obj_t *l, int16_t r, int16_t top) {
    place_right(l, L.card_w - r, top);
}

// Segura ante nulos: una placa sin columna de precio no recibe objeto para ella.
static void set_vis(lv_obj_t *l, bool on) {
    if (!l) return;
    if (on) lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
}

void pf_usage_init(lv_obj_t *parent) {
    compute_layout(board_caps());
    min_sym_w = (int16_t)text_w(PF_MIN_SYM, L.rows_font);

    root = make_group(parent);
    header_lbl = make_label(root, L.header_x, L.header_top, L.header_font, PF_MUTED);

    // La batería es de ui.cpp y se queda encima de esta pantalla, como en las
    // dos pantallas de consumo.
    card1 = make_card(root, L.c1_y, L.c1_h, L.c1_radius);
    card2 = make_card(root, L.c2_y, L.c2_h, L.c2_radius);

    // Tarjeta 1. "Valor total" (o "Cambio hoy" en privado) y "COP" enmarcan el
    // héroe por arriba; las dos columnas van debajo, con etiqueta o sin ella
    // según la placa. La placa chica no tiene etiqueta y muestra la variación
    // del día como una sola línea de 16 px en vez de dos columnas.
    lbl_label = make_label(card1, L.pad, L.label_top, L.label_font, PF_MUTED);
    lbl_unit  = make_label(card1, 0, L.unit_top, L.unit_font, PF_MUTED);
    place_right(lbl_unit, L.unit_ri, L.unit_top);
    lbl_hero  = make_label(card1, L.pad, L.hero_top, L.hero_font, PF_TEXT);
    if (L.has_cols) {
        lbl_col1 = make_label(card1, L.col1_x, L.col_label_top, L.col_label_font, PF_MUTED);
        lbl_col2 = make_label(card1, L.col2_x, L.col_label_top, L.col_label_font, PF_MUTED);
        lv_label_set_text(lbl_col1, "Cambio hoy");
        lv_label_set_text(lbl_col2, "Cambio %");
        lbl_dc     = make_label(card1, L.col1_x, L.col_val_top, L.col_val_font, PF_TEXT);
        lbl_dc_pct = make_label(card1, L.col2_x, L.col_val_top, L.col_val_font, PF_TEXT);
    } else {
        lbl_dc     = make_label(card1, L.day_dc_x, L.day_top, L.col_val_font, PF_TEXT);
        lbl_dc_pct = make_label(card1, 0, L.day_top, L.col_val_font, PF_TEXT);
        place_right(lbl_dc_pct, L.day_pct_ri, L.day_top);
    }
    for (int i = 0; i < 3; i++)
        lbl_e[i] = make_label(card1, L.pad, L.e1_top,
                              i == 0 ? L.e1_font : L.e2_font,
                              i == 0 ? PF_TEXT : PF_MUTED);

    // Tarjeta 2: las variaciones. El título y la cuenta de acciones comparten
    // línea, con la cuenta contra el relleno derecho.
    lbl_title  = make_label(card2, L.pad, L.title_top, L.title_font, PF_MUTED);
    lbl_count  = make_label(card2, 0, L.title_top, L.title_font, PF_MUTED);
    place_right(lbl_count, L.count_ri, L.title_top);
    lbl_norows = make_label(card2, L.pad, L.norows_top, L.norows_font, PF_MUTED);

    for (int i = 0; i < PF_ROWS; i++) {
        row_sym[i] = make_label(card2, L.f.sym_x, 0, L.rows_font, PF_TEXT);
        if (L.f.price_ri >= 0)
            row_price[i] = make_label(card2, 0, 0, L.rows_font, PF_MUTED);
        row_pct[i] = make_label(card2, 0, 0, L.rows_font, PF_TEXT);
        if (L.f.contrib_ri >= 0)
            row_contrib[i] = make_label(card2, 0, 0, L.rows_font, PF_TEXT);
    }

    // Línea de estado, en root y no dentro de una tarjeta: es así como el
    // usuario se entera de que los números de arriba se quedaron sin actualizar
    // o de que se cayó el enlace. El punto es la marca blanca fija de "En vivo"
    // — un círculo dibujado, no un glifo, y que nunca parpadea (un parpadeo es
    // un redibujado en la única pantalla construida para no redibujar).
    status_dot = lv_obj_create(root);
    lv_obj_set_pos(status_dot, L.st_x, L.st_dot_y);
    lv_obj_set_size(status_dot, L.st_dot, L.st_dot);
    lv_obj_set_style_bg_color(status_dot, PF_TEXT, 0);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_dot, 0, 0);
    lv_obj_set_style_radius(status_dot, L.st_dot / 2, 0);
    lv_obj_set_style_pad_all(status_dot, 0, 0);
    lv_obj_clear_flag(status_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(status_dot, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(status_dot, LV_OBJ_FLAG_HIDDEN);

    status_lbl = make_label(root, L.st_x, L.st_top, L.st_font, PF_MUTED);
    lv_label_set_text(status_lbl, "");

    // Estado neutro, para que la pantalla no quede en blanco si se muestra antes
    // del primer payload (ui.cpp la mantiene fuera del ciclo hasta entonces).
    memset(&cur, 0, sizeof(cur));
    cur.ok = true;
    redraw();

    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

// ---- Drawing ----------------------------------------------------------------

// En qué modo hay que dibujar la pantalla ahora mismo. El modo privado solo se
// aplica a un payload con datos: los estados vacíos no tienen nada privado que
// ocultar, así que conservan la caja de tarjeta normal y solo cambia el texto
// que revela `n`.
static bool want_private(void) {
    return pf_privacy_get() && cur.ok;
}

// La posición de la tarjeta 2 para el modo actual. Solo corre cuando el modo
// cambia de verdad, así que un redibujado con el modo intacto nunca toca una
// posición. La altura de la tarjeta 1 es de draw_panel1(), que también tiene que
// saber de los estados vacíos.
static void apply_mode(bool priv) {
    int m = priv ? 1 : 0;
    if (mode_applied == m) return;
    mode_applied = m;
    // El modo privado quita las dos columnas y la unidad de la tarjeta 1, así
    // que esa tarjeta se acorta y la tarjeta 2 sube en bloque, conservando su
    // propia altura (spec de privacidad §4). El hueco que queda debajo es la
    // señal visible del modo y nunca se rellena.
    lv_obj_set_pos(card2, L.card_x, priv ? L.pv_c2_y : L.c2_y);
}

// Dos zonas de aviso en la cabecera: "e" manda sobre "u", y "u" sobre "w"
// (spec de portfolio §4). Primary, siempre con el prefijo "!" y siempre sobre el
// fondo negro por encima de la primera tarjeta.
static void draw_header(void) {
    const char *text = "Portafolio";
    lv_color_t color = PF_MUTED;
    if (cur.ok && cur.u >= 1) {
        if (cur.ux[0]) {
            static char buf[40];
            if (cur.u > 1) snprintf(buf, sizeof buf, "! %s +%d sin precio", cur.ux, cur.u - 1);
            else            snprintf(buf, sizeof buf, "! %s sin precio", cur.ux);
            text = buf;
        } else {
            static char buf[24];
            snprintf(buf, sizeof buf, "! %d sin precio", cur.u);
            text = buf;
        }
        color = PF_PRIMARY;
    } else if (cur.ok && cur.w >= 1) {
        static char buf[32];
        snprintf(buf, sizeof buf, "! %d aviso, ver registro", cur.w);
        text = buf;
        color = PF_PRIMARY;
    }
    lv_label_set_text(header_lbl, text);
    lv_obj_set_style_text_color(header_lbl, color, 0);
}

// La tarjeta 1 en sus tres formas: un estado vacío, el héroe privado (el
// porcentaje del día, del mismo tamaño que el valor de mercado) o el valor de
// mercado con la variación del día debajo, en pesos y en porcentaje.
static void draw_panel1(bool priv) {
    int16_t h1 = !cur.ok ? L.e_h : (priv ? L.pv_c1_h : L.c1_h);
    if (h1 != card1_h) {
        card1_h = h1;
        lv_obj_set_size(card1, L.card_w, h1);
    }

    if (!cur.ok) {
        // La línea 1 dice qué pasó; las líneas 2 y 3, cómo arreglarlo.
        const char *l1, *l2, *l3;
        if (strcmp(cur.e, "nopos") == 0) {
            l1 = "Sin posiciones";
            // La ruta completa no cabe en la tarjeta de 240: allí
            // "config: portfolio".
            l2 = L.short_paths ? "config: portfolio"
                               : "~/.config/claude-usage-monitor/portfolio";
            l3 = "Formato: TICKER.CL cantidad";
        } else if (strcmp(cur.e, "nonet") == 0) {
            l1 = "Sin precios"; l2 = "Yahoo no responde"; l3 = "Reintenta cada 60 s";
        } else {   // nores
            l1 = "Sin precios"; l3 = "Revisa el sufijo .CL"; l2 = "";
            // Es el único estado vacío que reacciona al modo privado, y lo hace
            // aunque el payload venga con ok:false — "0 de 12 con precio"
            // delata n, que es privado (spec de privacidad §3.2). La tarjeta se
            // queda en la normal: aquí no hay nada más que ocultar.
            if (pf_privacy_get()) {
                l2 = "Ninguno con precio";
            } else {
                static char buf[24];
                snprintf(buf, sizeof buf, "0 de %d con precio", cur.n);
                l2 = buf;
            }
        }
        const char *lines[3] = { l1, l2, l3 };
        const int16_t tops[3] = { L.e1_top, L.e2_top, L.e3_top };
        set_vis(lbl_label, false);
        set_vis(lbl_unit, false);
        set_vis(lbl_hero, false);
        set_vis(lbl_dc, false);
        set_vis(lbl_dc_pct, false);
        set_vis(lbl_col1, false);
        set_vis(lbl_col2, false);
        for (int i = 0; i < 3; i++) {
            if (!lines[i] || !lines[i][0]) { set_vis(lbl_e[i], false); continue; }
            lv_label_set_text(lbl_e[i], lines[i]);
            lv_obj_set_pos(lbl_e[i], L.pad, tops[i]);
            lv_obj_set_style_text_color(lbl_e[i], i == 0 ? PF_TEXT : PF_MUTED, 0);
            set_vis(lbl_e[i], true);
        }
        return;
    }

    for (int i = 0; i < 3; i++) set_vis(lbl_e[i], false);

    // "Valor total" nombra el héroe; en modo privado no hay total, así que la
    // etiqueta dice lo que queda, el porcentaje del día. La unidad se va con él:
    // "COP" describiría un número que ya no está en pantalla.
    lv_label_set_text(lbl_label, priv ? "Cambio hoy" : "Valor total");
    set_vis(lbl_label, L.has_label);
    lv_label_set_text(lbl_unit, "COP");
    set_vis(lbl_unit, !priv);

    if (priv) {
        // El héroe es el porcentaje del día: es un ratio, así que no lleva
        // escala. Su signo sale de dc, que es la misma información que da el
        // color.
        lv_label_set_text(lbl_hero, fmt_pct_day(cur.dp, cur.dc));
        lv_obj_set_style_text_color(lbl_hero, money_color(cur.dc), 0);
    } else {
        // El valor de mercado es un nivel: sin color y sin signo.
        lv_label_set_text(lbl_hero, fmt_market_value(cur.mv));
        lv_obj_set_style_text_color(lbl_hero, PF_TEXT, 0);

        // Variación del día, en pesos y en porcentaje, mismo color y mismo
        // signo. En pesos es el número al que suman las contribuciones de la
        // lista, así que las dos se comparan cifra a cifra.
        lv_color_t c = money_color(cur.dc);
        lv_label_set_text(lbl_dc, fmt_cop_full(cur.dc));
        lv_obj_set_style_text_color(lbl_dc, c, 0);
        lv_label_set_text(lbl_dc_pct, fmt_pct_day(cur.dp, cur.dc));
        lv_obj_set_style_text_color(lbl_dc_pct, c, 0);
        set_vis(lbl_dc, true);
        set_vis(lbl_dc_pct, true);
    }
    set_vis(lbl_hero, true);
    set_vis(lbl_col1, !priv && L.has_cols);
    set_vis(lbl_col2, !priv && L.has_cols);
    if (priv) { set_vis(lbl_dc, false); set_vis(lbl_dc_pct, false); }
}

// Tarjeta 2: las variaciones más grandes. El daemon ya manda como mucho dos
// subidas y luego dos bajadas; el firmware descarta cualquier 0% exacto (una fila
// en 0% no dice nada y no es una variación) y reagrupa por signo para que el aire
// entre los dos grupos nunca caiga en medio de uno. Nunca una fila de relleno.
static void draw_panel2(bool priv) {
    set_vis(card2, cur.ok);
    if (!cur.ok) return;

    lv_label_set_text(lbl_title, "Mayores variaciones");
    char cbuf[16];
    snprintf(cbuf, sizeof cbuf, "de %d", cur.n);
    lv_label_set_text(lbl_count, cbuf);
    // "de 12" dice cuántas acciones se tienen: es privado, así que desaparece.
    set_vis(lbl_count, !priv);

    const PfRow *rows[PF_ROWS];
    int n = 0;
    for (int i = 0; i < cur.nr && n < PF_ROWS; i++) {
        if (cur.r[i].pct == 0) continue;
        rows[n++] = &cur.r[i];
    }
    // Partición estable: primero las subidas y luego las bajadas, conservando
    // cada una el orden que mandó el daemon (mejor % primero, más negativo
    // primero).
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && rows[j]->pct > 0 && rows[j - 1]->pct < 0; j--) {
            const PfRow *tmp = rows[j]; rows[j] = rows[j - 1]; rows[j - 1] = tmp;
        }
    }
    int g = 0;
    while (g < n && rows[g]->pct > 0) g++;

    const RowFields *f = priv ? &L.f_priv : &L.f;

    if (n == 0) {
        lv_label_set_text(lbl_norows, "Sin cambios por ahora");
        set_vis(lbl_norows, true);
        for (int i = 0; i < PF_ROWS; i++) {
            set_vis(row_sym[i], false); set_vis(row_price[i], false);
            set_vis(row_pct[i], false); set_vis(row_contrib[i], false);
        }
        return;
    }
    set_vis(lbl_norows, false);

    // Pasada 1: formatea cada fila y la mide con todas sus columnas puestas,
    // para encontrar el presupuesto de símbolo más ajustado de la pantalla.
    for (int i = 0; i < n; i++) {
        RowGeom *rg = &rowg[i];
        rg->w_price = 0; rg->w_contrib = 0;
        strlcpy(rg->sym, rows[i]->sym, sizeof rg->sym);
        strlcpy(rg->pct, fmt_pct_row(rows[i]->pct), sizeof rg->pct);
        rg->w_pct = (int16_t)text_w(rg->pct, L.rows_font);
        if (row_price[i]) {
            strlcpy(rg->price, fmt_price(rows[i]->price), sizeof rg->price);
            rg->w_price = (int16_t)text_w(rg->price, L.rows_font);
        }
        // La contribución depende de la cuenta de acciones, así que en modo
        // privado no se formatea ni se dibuja en ninguna de sus formas.
        if (row_contrib[i] && f->contrib_ri >= 0) {
            strlcpy(rg->contrib, fmt_cop_full(rows[i]->contrib), sizeof rg->contrib);
            rg->w_contrib = (int16_t)text_w(rg->contrib, L.rows_font);
        }
    }

    // El guardián (spec §2.3.1), decidido una sola vez para toda la pantalla para
    // que las columnas se queden en el mismo sitio de fila a fila: si lo que
    // sobra no puede albergar el símbolo más corto que merece mostrarse, se va la
    // contribución y luego el precio. Ninguna placa puede acabar con un número
    // dibujado encima de otro, ni con un símbolo de menos de tres letras.
    bool show_c = f->contrib_ri >= 0;
    bool show_pr = f->price_ri >= 0;
    if (tightest_budget(f, n, show_c, show_pr) < min_sym_w) {
        show_c = false;
        if (tightest_budget(f, n, show_c, show_pr) < min_sym_w) show_pr = false;
    }

    // Pasada 2: coloca las columnas que sobreviven y corta cada símbolo a su
    // propio presupuesto.
    for (int i = 0; i < n; i++) {
        RowGeom *rg = &rowg[i];
        int16_t budget = row_layout(rg, f, show_c, show_pr,
                                    &rg->r_contrib, &rg->r_pct, &rg->r_price);
        rg->top = L.rows_top + i * L.rows_pitch
                + ((g > 0 && i >= g) ? L.rows_gap : 0);
        lv_color_t c = money_color(rows[i]->pct);

        lv_obj_set_pos(row_sym[i], f->sym_x, rg->top);
        set_vis(row_sym[i], budget >= min_sym_w);
        if (budget >= min_sym_w) {
            char cut[PF_SYM_MAX + 2];
            lv_label_set_text(row_sym[i], fmt_sym(rg->sym, budget, cut, sizeof cut));
        }

        // El precio es la cifra de referencia, así que es la única columna en
        // muted y la única sin signo: "número gris sin signo = precio" es lo que
        // lo distingue de la variación que lleva al lado.
        set_vis(row_price[i], show_pr);
        if (show_pr) {
            lv_label_set_text(row_price[i], rg->price);
            lv_obj_set_style_text_color(row_price[i], PF_MUTED, 0);
            place_at(row_price[i], rg->r_price, rg->top);
        }

        lv_label_set_text(row_pct[i], rg->pct);
        lv_obj_set_style_text_color(row_pct[i], c, 0);
        place_at(row_pct[i], rg->r_pct, rg->top);
        set_vis(row_pct[i], true);

        set_vis(row_contrib[i], show_c);
        if (show_c) {
            lv_label_set_text(row_contrib[i], rg->contrib);
            lv_obj_set_style_text_color(row_contrib[i], c, 0);
            place_at(row_contrib[i], rg->r_contrib, rg->top);
        }
    }
    for (int i = n; i < PF_ROWS; i++) {
        set_vis(row_sym[i], false); set_vis(row_price[i], false);
        set_vis(row_pct[i], false); set_vis(row_contrib[i], false);
    }
}

// Estado de la sesión y del enlace, en la línea de estado (spec de portfolio
// §4). El aviso transitorio de "Modo privado" / "Modo normal" tiene prioridad:
// es lo que le explica a un usuario con el hábito de "PWR = brillo" por qué la
// pantalla acaba de cambiar.
static void draw_status(uint32_t now) {
    char buf[48];
    const char *text;
    lv_color_t color;
    bool dot = false;

    if (pf_privacy_notice()) {
        text = pf_privacy_get() ? "Modo privado" : "Modo normal";
        color = PF_MUTED;
    } else if (!ble_on) {
        text = "! Bluetooth desconectado";
        color = PF_PRIMARY;
    } else if (!cur.ok) {
        text = "";                    // los estados vacíos no tienen nada que contar
        color = PF_MUTED;
    } else if (cur.live) {
        bool stale = have_data && (now - data_ms) > PF_FRESH_MS;
        if (stale) {
            // Deja de decir "En vivo", que era lo único falso en pantalla. Nada
            // se atenúa: en la bolsa, "viejo" es el estado normal.
            snprintf(buf, sizeof buf, "Sin actualizar desde %s", fmt_clock(cur.t));
            text = buf;
            color = PF_MUTED;
        } else {
            snprintf(buf, sizeof buf, "En vivo, %s", fmt_clock(cur.t));
            text = buf;
            color = PF_TEXT;
            dot = true;
        }
    } else {
        snprintf(buf, sizeof buf, "Cierre %s", fmt_session_date(cur.t));
        text = buf;
        color = PF_MUTED;
    }

    if (strcmp(text, status_text) != 0) {
        lv_label_set_text(status_lbl, text);
        strlcpy(status_text, text, sizeof(status_text));
    }
    if (!lv_color_eq(color, status_color)) {
        status_color = color;
        lv_obj_set_style_text_color(status_lbl, color, 0);
    }
    // El estado activo sangra el texto para despejar el punto; todos los demás
    // estados arrancan en el margen de la pantalla.
    int16_t x = dot ? L.st_text_x : L.st_x;
    if (x != status_x) {
        status_x = x;
        lv_obj_set_pos(status_lbl, x, L.st_top);
    }
    if (dot != sq_shown) {
        sq_shown = dot;
        if (dot) lv_obj_clear_flag(status_dot, LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(status_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

// Un enlace caído es el único caso que atenúa: allí la frescura propia del dato
// es desconocida. Todo lo demás de esta pantalla se mantiene a brillo completo.
// Al 40% la tarjeta #1a1a1a cae sobre el negro más o menos en #0a0a0a, y el
// precio se atenúa con ella a propósito: es la cifra que se vuelve vieja más
// rápido.
static void apply_dim(void) {
    bool dim = !ble_on;
    if (dim == dimmed) return;
    dimmed = dim;
    // opa se hereda, así que las dos tarjetas se apagan en bloque mientras la
    // cabecera y la línea de estado mantienen la opacidad completa.
    lv_obj_set_style_opa(card1, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
    lv_obj_set_style_opa(card2, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
}

static void redraw(void) {
    if (!root) return;
    bool priv = want_private();
    apply_mode(priv);
    draw_header();
    draw_panel1(priv);
    draw_panel2(priv);
    draw_status(millis());
    apply_dim();
}

// ======== API pública =========================================================

void pf_usage_show(void) {
    if (!root) return;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    redraw();
}

void pf_usage_hide(void) {
    if (!root) return;
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *pf_usage_get_root(void) {
    return root;
}

void pf_usage_update(const PfData *d) {
    if (!d) return;
    cur = *d;
    data_ms = millis();
    if (d->valid) have_data = true;
    redraw();
}

void pf_usage_set_ble(bool connected) {
    ble_on = connected;
    redraw();
}

// Corre desde ui_tick_anim() mientras esta pantalla está visible. Dos cosas
// dependen del tiempo — el aviso transitorio del modo privado y el cambio a "Sin
// actualizar" — más el conmutador de PWR, que tiene que redibujar desde el
// indicador de modo. Con el mercado cerrado y sin que nadie toque nada, esto no
// empuja nada a LVGL.
void pf_usage_tick(void) {
    if (!root) return;
    if (want_private() != (mode_applied == 1)) {
        redraw();
        return;
    }
    draw_status(millis());
    apply_dim();
}

bool pf_has_data(void) {
    return have_data;
}
