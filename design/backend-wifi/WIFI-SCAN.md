# Listado de redes WiFi en el portal cautivo — diseño

**Estado:** aprobado para implementación.
**Alcance:** el portal SoftAP de aprovisionamiento (`firmware/src/portal.cpp`).
Hoy el portal pide el **SSID a mano**; este cambio muestra las **redes WiFi
detectadas** para que el usuario elija una, con la entrada manual como respaldo.
**Contrato previo:** `design/backend-wifi/ROADMAP.md` §7 (SoftAP + portal
cautivo). Este documento lo extiende; no lo reemplaza.

---

## 1. Objetivo

En la página que sirve el dispositivo:

1. Al abrir, **escanear** las redes cercanas automáticamente.
2. Listar las redes con **intensidad de señal** y si están **protegidas**.
3. **Tocar una red** rellena el campo SSID (el usuario sólo escribe la
   contraseña).
4. Un botón **Actualizar** vuelve a escanear.
5. La **entrada manual** del SSID sigue existiendo (redes ocultas, listas
   incompletas, SSID que no aparece).
6. La página pasa a **español** (el resto del firmware no cambia de idioma).

Nada del aprovisionamiento cambia por debajo: al guardar se sigue llamando a
`ota_set_wifi()`, que es el único camino de escritura a NVS (`ota.h`). El
emparejamiento (`usage_pair_code()`) y el `http://192.168.4.1` de respaldo no
cambian.

## 2. Decisiones

| # | Decisión | Por qué |
|---|---|---|
| W1 | Escaneo **asíncrono**, iniciado por el cliente | `WiFi.scanNetworks()` bloquea 2–4 s. Sincrónico, el handler HTTP deja sin servicio DNS/HTTP al portal todo ese rato. Async parte el escaneo y el cliente hace *polling* corto. |
| W2 | El portal sube a `WIFI_AP_STA`, no `WIFI_AP` | En `WIFI_AP` puro el driver no tiene interfaz STA para escanear y `scanNetworks()` falla. `AP_STA` mantiene el SoftAP arriba y habilita el escaneo. |
| W3 | El listado va en la misma página (sin recargar) | Menos round-trips, sin estado de servidor, y el formulario no pierde lo escrito. |
| W4 | Se conserva el campo SSID manual | El escaneo nunca es la única vía (red oculta, SSID especial, escaneo vacío). |
| W5 | Spanish UI | Decisión del usuario (08-10-2026). Los *comentarios* del código siguen en inglés como el resto del firmware. |
| W6 | `portal.h` no cambia de API | Todo es interno a `portal.cpp`; el sim (que excluye `portal.cpp`) no se toca. |

## 3. Modelo de radio

- `portal_start_now()` pasa de `WiFi.mode(WIFI_AP)` a `WiFi.mode(WIFI_AP_STA)`
  y luego `WiFi.softAPConfig(...)` / `WiFi.softAP(ssid)`. `softAPConfig` debe
  llamarse **después** de fijar el modo, como está hoy.
- `portal_stop_now()` y el cierre por OTA siguen llamando
  `WiFi.softAPdisconnect(true)` + `WiFi.mode(WIFI_OFF)`. `WIFI_OFF` cancela un
  escaneo en curso; además hay que **resetear el estado de escaneo** en el
  teardown (ver §6).
- El driver WiFi ya está pre-inicializado en `setup()` (`main.cpp`), así que
  `WIFI_AP_STA` no trae una inicialización nueva.

## 4. Contrato HTTP

Todas las rutas responden en el loop task, dentro de `portal_tick()` →
`s_http.handleClient()`. `WiFi` nunca se toca desde la tarea NimBLE.

### `GET /scan?start=1`

Arranca (o reusa) un escaneo asíncrono.

- Si ya hay uno corriendo: no arranca otro.
- Si no: `WiFi.scanNetworks(true, /*show_hidden=*/false)`.
- Código 200, `application/json`.

```json
{"state":"scanning"}
```

### `GET /scan`

Consulta el estado del escaneo asíncrono (`WiFi.scanComplete()`).

- `resultado < 0` (corriendo o nunca iniciado) → `{"state":"scanning"}`.
- `resultado >= 0` → arma el listado, llama `WiFi.scanDelete()` y devuelve:

```json
{"state":"done","nets":[{"ssid":"Casa","rssi":-45,"sec":true},{"ssid":"Cafe","rssi":-72,"sec":false}]}
```

Reglas del listado:

- **Orden:** RSSI descendente (más fuerte primero).
- **Deduplicado** por SSID, conservando el RSSI más alto.
- **Descartar** SSIDs vacíos (redes ocultas: no se pueden elegir por nombre).
- **Tope:** 24 redes.
- `sec` es `true` salvo `WIFI_AUTH_OPEN` (`WiFi.encryptionType(i)`): la
  contraseña se muestra siempre, pero el candado informa.
- El SSID se **escapa como JSON** (comillas, barra invertida y control
  characters). Un SSID es un byte-string; se puede asumir UTF-8 de buena fe,
  pero nunca se interpola crudo.

El resultado se entrega con `setContentLength` calculado y `sendContent`
enviado **por filas**, con un buffer de pila chico (`char row[160]`), para no
materializar todo el JSON como un `String` de heap. La lista de resultados
sigue viva en el driver entre las dos pasadas; `scanDelete()` va después de
streamear.

### Estado interno (en `portal.cpp`)

```c
static uint32_t s_scan_started_ms = 0;   // 0 = sin escaneo en curso
#define PORTAL_SCAN_STALE_MS 20000u      // escaneo colgado → se descarta
```

- Si `scanComplete()` sigue `< 0` pasado `PORTAL_SCAN_STALE_MS` desde el
  arranque, `WiFi.scanDelete()`, resetear `s_scan_started_ms = 0` y devolver
  `{"state":"done","nets":[]}` (el cliente muestra "no se encontraron redes").
- `WiFi.scanDelete()` es inocuo si no hay resultados.

## 5. Página y JavaScript

### 5.1 Estructura (`PORTAL_HTML_TAIL`, en flash)

```html
<h1>Configurar Clawdmeter</h1>
<p>Conecta este dispositivo a tu red WiFi.</p>
<!-- bloque de código de emparejamiento (dinámico, sin cambios) -->
<section class="nets">
  <div class="nets-head">
    <span>Redes disponibles</span>
    <button type="button" id="rescan">Actualizar</button>
  </div>
  <div id="nets" class="nets-list"><p class="nets-msg">Buscando redes…</p></div>
</section>
<form method="POST" action="/save">
  <label for="ssid">Red (SSID)</label>
  <input id="ssid" name="ssid" maxlength="63" required autocapitalize="off"
         autocorrect="off" autocomplete="off">
  <label for="pass">Contraseña</label>
  <input id="pass" name="pass" type="password" maxlength="63">
  <button type="submit">Guardar y conectar</button>
</form>
<p class="s">Guardado solo en este dispositivo. Si esta página no se abre sola,
visita <b>http://192.168.4.1</b></p>
```

El bloque de emparejamiento y su texto pasan a español:

- Encabezado: `Código de emparejamiento`
- Ayuda: `Introduce este código con la herramienta del propietario para emparejar el dispositivo.`

### 5.2 Comportamiento (JS inline, en flash)

- Al `DOMContentLoaded`: `startScan()`.
- `startScan()`: `fetch('/scan?start=1')`, pinta `Buscando redes…` y hace
  *polling* a `/scan` cada **500 ms**, hasta **20 intentos** (10 s). Al recibir
  `state:"done"`: renderiza la lista o el mensaje `No se encontraron redes`.
  Si se agota el tiempo: `No se pudo escanear. Intenta de nuevo.`
- Botón `#rescan`: deshabilitado mientras busca; re-ejecuta `startScan()`.
- Render de una fila: `button.net` (todo el renglón es tocable) con
  `textContent` para el SSID (**nunca** `innerHTML` con datos del escaneo),
  barras de señal por RSSI y un candado textual si `sec`.
- Al tocar una fila: `#ssid.value = ssid`, foco en `#pass`, y se marca la fila
  activa (`.on`). El campo SSID queda **editable**: el usuario puede corregirlo.
- Barras de señal (4 niveles): RSSI `>= -55` → 4, `>= -67` → 3, `>= -75` → 2,
  si no 1. Se pintan con spans en CSS, sin imágenes.
- Si `fetch`/JSON falla: `No se pudo escanear. Intenta de nuevo.` y el botón
  vuelve a habilitarse.

### 5.3 Estilos (sumar a `PORTAL_HTML_HEAD`)

Clases nuevas: `.nets`, `.nets-head`, `.nets-list`, `.net`, `.net.on`,
`.net .ssid`, `.net .sig`, `.net .sig i`, `.lock`, `.nets-msg`. Mantener la
paleta actual (`#0d1117`/`#161b22`/`#30363d`/`#238636`). Las filas son
`button` sin borde por defecto, con hover/focus visibles. En pantallas con
las `22rem` de ancho, un SSID largo se recorta con `overflow:hidden;
text-overflow:ellipsis; white-space:nowrap`.

### 5.4 Páginas restantes en español

| Página | Título | Cuerpo |
|---|---|---|
| Guardado | `Guardado ✓` | `Conectando a tu red… puedes cerrar esta pestaña.` |
| Error | `Falta el nombre de la red` | `Escribe el nombre de tu red.` + `Reintentar` (enlace a `/`) |

## 6. Interacción con OTA y cierre

- El cierre por OTA activo y `portal_stop_now()` deben poner
  `s_scan_started_ms = 0` (y `WiFi.scanDelete()` si hace falta). `WIFI_OFF`
  corta el escaneo de todos modos, pero el estado tiene que quedar limpio para
  el próximo `portal_start()`.
- Sigue prohibido que el portal corra mientras `ota_is_active()` o
  `ota_pull_is_active()`; `portal_tick()` ya cierra el portal en ese caso.

## 7. Memoria y threading (invariantes)

- **Todo** el HTML/CSS/JS nuevos viven en `.rodata` (`PROGMEM`) y se streamean;
  no se agregan `String` de heap salvo el bloque de emparejamiento (que ya
  existía) y los `arg()` del POST.
- El handler `/scan` usa un buffer de pila por fila, no un buffer global grande.
- `WiFi.scanNetworks()`/`scanComplete()`/`scanDelete()` corren **sólo** en el
  loop task. Nunca desde la tarea NimBLE.
- No se agrega ningún `#ifdef BOARD_*` en código compartido.

## 8. Archivos

| Archivo | Cambio |
|---|---|
| `firmware/src/portal.cpp` | Todo el cambio: modo `AP_STA`, handler `/scan`, estado de escaneo, HTML/CSS/JS, textos en español. |
| `firmware/src/portal.h` | Sin cambios de API (sólo, si acaso, un comentario). |
| `design/backend-wifi/WIFI-SCAN.md` | Este documento. |
| `design/backend-wifi/ROADMAP.md` | §7 apunta a este documento. |

## 9. Verificación

1. **Compila:** `pio run -d firmware -e waveshare_amoled_216` (el sim excluye
   `portal.cpp`, así que además `pio run -d firmware -e sim` no debe romperse).
2. **Revisión contra el contrato:** `/scan?start=1` y `/scan` devuelven los
   estados de §4; el JSON de `done` está bien formado y ordenado.
3. **Manual (en placa, no se puede hacer en CI):**
   - Borrar credenciales → el portal se levanta solo a los ~5 s.
   - Entrar a `http://192.168.4.1` desde el teléfono: aparece la lista.
   - Tocar una red: rellena el SSID y enfoca la contraseña.
   - Guardar: llega a la página "Guardado ✓" y el dispositivo se conecta.
   - Con la red oculta: el campo manual sigue funcionando.
   - `Actualizar` re-escanea.
4. **Regresión:** el código de emparejamiento se sigue mostrando cuando el
   dispositivo no está emparejado; el fallback `192.168.4.1` sigue en la página.
