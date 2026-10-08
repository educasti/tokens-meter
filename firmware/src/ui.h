#pragma once
#include "data.h"
#include "ble.h"
#include "oc_data.h"
#include "pf_data.h"

// Ciclo de navegación (SPEC.md §6): splash de Clawd → uso de Claude → splash de
// OpenCode → uso de OpenCode → portfolio de BVC → y vuelta al splash de Clawd.
// Las dos pantallas de OpenCode quedan fuera del ciclo hasta que aterriza el
// primer payload de OpenCode, y el portfolio hasta el primero de "k":"pf", así
// que un dispositivo que nunca recibe ninguno de los dos se comporta igual que
// antes.
// Pantallas del ciclo de navegación. El Command Center NO está aquí: es un
// overlay fuera del ciclo (ver ui_sys.h) que se abre con gesto desde cualquier
// pantalla y se cierra con gesto o toque, sin tocar ciclo, dots ni navegación.
enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_OC_SPLASH,
    SCREEN_OC_USAGE,
    SCREEN_PORTFOLIO,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
// Igual que ui_update(), pero marca que los datos vienen del pull del backend
// WiFi y no del daemon BLE, así que usa la ventana de frescura externa, más
// larga, incluso con BLE conectado. (ui_update() ya trata como externa una
// actualización que llega con BLE desconectado.)
void ui_update_external(const UsageData* data);
void ui_update_opencode(const OcData* data);
void ui_update_portfolio(const PfData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_next_screen(void);
void ui_prev_screen(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);

// Línea transitoria de estado de OTA por pull (DESIGN.md §12). `pct < 0` quiere
// decir "sin porcentaje". Reutiliza el área de estado animada —nunca agrega una
// pantalla— y se borra sola tras unos segundos. No depende del transporte y se
// puede llamar desde cualquier tarea (p. ej. el worker del pull de OTA): el texto
// se copia bajo una sección crítica mínima y lo dibuja ui_tick_anim() en la tarea
// del loop; esta llamada nunca toca LVGL y nunca bloquea.
void ui_ota_status(const char* text, int pct);
