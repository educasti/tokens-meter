# Command Center: especificación (v1)

**Estado:** propuesta aprobada por el usuario 2026-10-08. Prototipo interactivo:
`design/device-prototype/prototype.html` (pantalla `sys` + sección B5).
**Glosario:** `docs/glosario-es.md` (normativo para todas las cadenas).

## 1. Objetivo

Agregar un **Command Center**: overlay de dashboard del propio dispositivo que
muestra radios, versión de firmware y estado OTA pull, con progreso de
descarga. **No es una pantalla del ciclo**: se abre con gesto y se cierra con
gesto o toque. El ciclo, los dots y la navegación no cambian.

## 2. Decisiones del usuario (08-10-2026)

1. Siempre accesible; nunca entra al ciclo (dots siguen máx 5).
2. PWR corto = comprobar/aplicar; **doble PWR confirma** la aplicación.
3. Sin WiFi, el PWR abre el portal (botón hacia el portal).
4. La descarga muestra velocidad y tiempo restante, además de barra + %.
5. Nombre propio: **Command Center** (se queda en inglés).

## 3. Comportamiento

### 3.1 Apertura

- Swipe down que **empieza en el borde superior** (`y < 60` en 480×480),
  dirección BOTTOM, desde **cualquier pantalla** (incluidos splashes).
- Implementación LVGL: en `LV_EVENT_PRESSED` guardar `y`; en
  `LV_EVENT_GESTURE` con `lv_indev_get_gesture_dir() == LV_DIR_BOTTOM`
  y `press_y < 60` → `sys_show()`. El handler vive en `ui.cpp` (nivel ciclo),
  no en cada pantalla. El touch del sim (SDL) también genera gestos: verificable.
- Si el Command Center ya está abierto, el gesto se ignora.

### 3.2 Cierre (overlay, no navega)

- Swipe up en cualquier `y`, o tap sobre el overlay → `sys_hide()`.
- Tap BOOT/SEC con overlay abierto: **solo cierra**, no navega.
- El tap normal (sin overlay) sigue avanzando el ciclo.
- Los dots **no** se muestran ni cambian; el overlay no llama `show_page_dots()`.
- `sys_hide()` desarma la confirmación pendiente (§5).

### 3.3 Visibilidad

- El overlay es un contenedor opaco a pantalla completa, hijo topmost de la
  pantalla activa (`lv_screen_active()`); al cerrar se oculta, la pantalla de
  abajo queda intacta (no se reconstruye nada).
- Batería: visible (es pantalla de datos; reutiliza `apply_battery_visibility()`
  con la regla "splash la oculta", el overlay no es splash).
- Mascota/logo: no se tocan (solo viven en uso de Claude; el overlay los tapa).

### 3.4 PWR con overlay abierto

- PWR corto → `sys_pwr_action()` (§5). Nunca brillo aquí.
- PWR 3 s + release → emparejar (intacto, `pair_tick()` independiente).
- Si el portal está activo y el overlay abre: PWR → `brightness_cycle()`
  (la radio está ocupada, no hay OTA posible).

## 4. Layout (480×480, corte grande)

Mismo lenguaje que portafolio: fondo negro, cards `#1a1a1a` radio 12,
Plex Mono (ya en firmware con glifos ES). Cortes compacto/chico: mismos 3
breakpoints que `ui_portfolio.cpp`, estructura idéntica, geometría a criterio.

| Elemento | Geometría |
|---|---|
| Header `Command Center` | x20, y36, Plex 16 muted |
| Batería (la de `ui.cpp`) | se conserva arriba a la derecha |
| Card 1 (versión + OTA) | x20, y72, w440, h162 |
| Card 2 (radios) | x20, y246, w440, h182 |
| Hint | y436, Plex 14 muted |
| Estado | y458, Plex 16 |

Card 1: etiqueta `Versión` + board id a la derecha (Plex 12),
héroe versión (Plex 48), línea OTA (Plex 18), caja de acción o barra.
Card 2: título `Radios y batería`; filas `BLE / WiFi / Batería / Portal WiFi`
(Plex 16, etiqueta muted izq., valor der.). La 4ª fila es tocable: valor
`Abrir` (portal apagado) / `Cerrar` (portal activo); el tap alterna
`portal_start()` / `portal_stop()` sin cerrar el overlay (sin burbuja al
handler de cierre) y el tick redibuja al cambiar `portal_is_active()`.
La fila WiFi sigue mostrando `Portal 192.168.4.1` cuando el portal está
activo.

## 5. PWR (`sys_pwr_action()`, desde `main.cpp`)

```
si portal activo:            brightness_cycle(); return
si pull/hybrid OTA activo:   nada (la pantalla ya muestra progreso); return
si !portal_has_creds():      portal_start(); return     // sin WiFi → al portal
si resultado == available && !armado:  armar (deadline +3.5 s en sys_tick); return
si armado:                   desarmar; ota_pull_request_apply(); return
si no:                       ota_pull_request_check(); return
```

## 6. Datos (lectura en vivo en `sys_tick()`, tarea del loop)

| Campo | Origen | Formato |
|---|---|---|
| Versión | `ota_version()` | verbatim (`v0.2.11`, sim `sim`) |
| Board | `board_caps().id` | verbatim |
| BLE | `ble_get_state()` | Conectado (verde) / Disponible (ámbar) / Desconectado (rojo) |
| WiFi portal | `portal_is_active()` | `Portal 192.168.4.1` (verde) |
| WiFi STA | `ota_wifi_connected()` + `ota_wifi_rssi()` | `SSID -52 dBm` (verde) |
| WiFi down | — | `Apagado` (rojo) |
| Batería | `power_hal_battery_pct()` + `is_charging()` | `78%`; etiqueta `Batería (cargando)` si carga; `—` si pct<0 |
| OTA vivo | `ota_pull_is_active()` + `ota_pull_state_name()` + `ota_is_active()` | Comprobando… / Descargando % / Verificando… / Reiniciando… / `OTA en curso` (híbrida) |
| OTA resultado | `ota_pull_ui_snapshot()` (§7) | Al día / Nueva / error / Sin comprobar |
| Edad | `snapshot.ms` vs `millis()` | `ahora mismo` (<60 s) / `hace 35 s, 21 min, 3 h 4 min` |

Prioridad de la línea OTA: vivo > resultado > `Sin comprobar`.
Tamaño: `1.4 MB` / `823 KB` (`%lu KB`, `%.1f MB` por `snprintf`).
Velocidad: `%lu KB/s` (`%.1f MB/s` si ≥1 MB/s); ETA: `quedan ~%lu s`;
si tasa 0, solo barra + %.

## 7. APIs nuevas (contrato exacto para implementar)

```c
// ota_pull.h  (requiere <stdint.h>; ya añadido en el worktree)
enum ota_ui_result_t { OTA_UI_UNKNOWN, OTA_UI_UP_TO_DATE,
                       OTA_UI_AVAILABLE, OTA_UI_ERROR, OTA_UI_REBOOTING };
struct OtaUiSnapshot {
    uint8_t  result;      // ota_ui_result_t
    char     ver[16];     // versión disponible, "" si ninguna
    long     size;        // bytes de la imagen
    int      pct;         // último %, -1 si ninguno
    uint32_t rate_bps;   // última tasa medida, 0 desconocida
    uint32_t eta_s;      // segundos restantes, 0 desconocido
    char     err[16];     // código corto, "" si ninguno
    uint32_t ms;          // millis() de la última actualización, 0 = nunca
};
void ota_pull_ui_snapshot(OtaUiSnapshot* out);  // copia bajo mux; loop task
void ota_pull_request_check(void);  // encola check, from_ble=false; loop task
void ota_pull_request_apply(void);  // encola apply, from_ble=false; loop task
```

Espejo en `ota_pull.cpp`: actualizar en cada `pull_send_*`
(`checking` limpia pct/tasa/err y conserva el resultado previo;
`up_to_date/available/verifying/rebooting/err` fijan resultado+`ms`;
**no** reflejar `busy`/`bad_json`: son respuestas de protocolo, no resultados).
Tasa/ETA en el loop de descarga junto a `pull_send_downloading(pct)`:
`bps = total*1000/elapsed` (math `uint64_t`), `eta = restante/bps`.

```c
// ota.h
const char* ota_stored_ssid(void);  // credencial NVS "otah", "" si ninguna
bool ota_wifi_connected(void);      // STA asociado ahora mismo
int  ota_wifi_rssi(void);           // dBm, 0 si caído
```

Stubs sim en `boards/sim/ota_sim.cpp`: snapshot cero, requests no-op,
ssid `""`, connected false, rssi 0.

## 8. Cadenas ES exactas (Plex cubre ASCII+acentos+`·`+`…`; prohibidos ●○▲—)

`Command Center`, `Versión`, `Radios y batería`, `BLE`, `WiFi`, `Batería`,
`Batería (cargando)`, `Portal WiFi`, `Abrir`, `Cerrar`, `Conectado`, `Disponible`, `Desconectado`,
`Portal 192.168.4.1`, `Apagado`, `Al día`, `Nueva: %s · %s`,
`Comprobando…`, `Descargando %s… %d%%`, `Verificando…`, `Reiniciando…`,
`OTA en curso`, `Sin WiFi - no comprobable` (guion ASCII, no em-dash),
`Sin comprobar`, `Última comprobación %s`, `ahora mismo`,
`PWR: comprobar ahora`, `PWR: aplicar actualización`,
`¿Aplicar %s? PWR confirma`, `PWR: abrir portal WiFi`,
hints `Toque: cerrar · PWR: …` / `No apagues · …` / `PWR: confirmar · Toque: cerrar`.
Errores: `timeout`→`Sin respuesta`, `tls_fail`→`Error TLS`,
`http_404`→`Sin versión`, `no_wifi`→`Sin WiFi`, `busy`→`Ocupado`,
`battery_low`→`Batería baja`, resto→`No se actualizó` (glosario).

## 9. Restricciones técnicas

- Sin `#ifdef BOARD_*` en código compartido (patrón `BoardCaps`/stubs sim).
- Lazy init como portafolio (heap LVGL ~48 KB ya ocupados); `sys_hide()` libera
  el armado pero no los widgets.
- Redibujar solo ante cambios (cachear último snapshot pintado).
- LVGL solo desde la tarea del loop; snapshot con `portMUX` (patrón `ota_status`).
- Verificar anchos con `tools/check_label_widths.py` (SSID hasta 63: truncar a
  ~18 + `.`, patrón `fmt_sym` del portafolio).

## 10. Archivos y cableado

1. `ota_pull.h/.cpp` + `ota.h/.cpp` + `ota_sim.cpp`: §7.
2. Nuevo `ui_sys.h/.cpp`: `sys_init_lazy()`, `sys_show()`, `sys_hide()`,
   `sys_is_open()`, `sys_tick()`, `sys_pwr_action()`.
3. `ui.h/.cpp`: overlay topmost (NO entra a `screen_t`/ciclo/dots);
   hook `sys_tick()` en `ui_tick_anim()`; handlers PRESS/GESTURE (§3.1);
   tap con overlay = cerrar (sin burbuja al `global_click_cb`).
4. `main.cpp`: `case` PWR → `sys_pwr_action()` si overlay abierto
   (vía `sys_is_open()`), resto intacto.
5. Sim: `SIM_START_SCREEN=sys` en `ble_sim.cpp` (abrir overlay tras payloads).
6. Docs: `CLAUDE.md` + `README.md` (ciclo/gesto/PWR); prototipo: ver §11.

## 11. Prototipo (actualizar `design/device-prototype/prototype.html`)

Sacar `sys` del ciclo (`getScreenList`), dots máx 5, abrir con drag-down
desde el borde superior en el sim, cerrar con drag-up/tap, subtítulos B5/A.

## 12. Verificación

```bash
pio run -d firmware -e sim && pio run -d firmware -e waveshare_amoled_216
python3 tools/check_label_widths.py  # (flags según --help)
SIM_START_SCREEN=sys SDL_VIDEODRIVER=dummy SIM_AUTOSHOT_MS=6000 firmware/.pio/build/sim/program
python -m pytest daemon/tests -q  # sin cambios en daemon, debe seguir verde
```
