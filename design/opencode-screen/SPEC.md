# Pantallas de OpenCode: especificación y diseño (v0.2)

**Estado:** implementado (rama `feat/opencode-screens`, commit `5b87b01`), verificado en hardware (Waveshare AMOLED-2.16; véase `design/opencode-screen/research/hw1-test-216.md`).
**Investigación:** `research/01` (datos locales), `02` (marca), `03` (firmware), `07` (límites de Go), `08` (animaciones oficiales) y `09` (endpoint verificado en vivo).

**Decisiones del usuario (28-09-2026):**
- Plan OpenCode Go de $10.
- Navegación con los botones laterales: toque corto para navegar, mantener conserva la tecla.
- Fuente a mi criterio: IBM Plex Mono.
- Incluir un splash de OpenCode.

---

## 1. Objetivo

Agregar al Clawdmeter **dos pantallas de OpenCode**, un splash animado y una de uso, con la identidad visual de OpenCode. Las pantallas de Claude no cambian. Si nunca llegan datos de OpenCode, el dispositivo se comporta exactamente como hoy.

## 2. Datos

### 2.1 Fuente principal: endpoint oficial de OpenCode Go (verificado en vivo)

`GET https://opencode.ai/zen/go/v1/usage/`, con `Authorization: Bearer <clave opencode-go>`.

- **La barra final es obligatoria:** sin ella el servidor devuelve 401.
- La clave está en `~/.local/share/opencode/auth.json`, campo `opencode-go.key`. **Nunca se registra en logs.**
- Responde en ~0,5 s con `{"usage":{"rolling"|"weekly"|"monthly":{"status","percent","resetsAt"}}}`.

Qué significa cada ventana:
- **5 h:** empieza con la primera petición y dura 5 h desde ahí. Con `percent = 0`, el servidor devuelve `resetsAt = ahora + 5 h`, que significa que no hay ventana activa.
- **Semana:** lunes 00:00 UTC.
- **Mes:** aniversario de la suscripción. En tu caso es el día 12, a las 17:04 UTC.
- `status ≠ "ok"` significa que se alcanzó el límite.

El porcentaje del servidor ya pondera cada modelo por su límite (Go tiene límites por modelo de $15, $30 y $60). Los modelos gratis no cuentan.

### 2.2 Respaldo: estimación local

Se usa si falla el endpoint (sin red, 401/403/5xx o sin clave). Se calcula desde `opencode.db` en solo lectura: `Σ(costo_paso / límite_del_modelo) / fracción_de_la_ventana`, con fracción 0,2 para 5 h, 0,5 para la semana y 1 para el mes. En pantalla lleva la marca `est.`

Es aproximada: ya se midió 2% local contra 3% del servidor, porque no ve el uso de otras máquinas ni los precios de hora pico.

### 2.3 Actividad local (siempre de `opencode.db`)

- Tokens de 7 días.
- Modelo principal y su porcentaje de tokens.
- Sesiones activas: actualizadas en los últimos 10 min.
- Agente de la sesión más reciente.
- Segundos desde la última actividad.

### 2.4 Sin OpenCode Go

Si no hay clave `opencode-go`, se usa el modo **solo consumo**: tokens y costo de hoy y de 7 días, sin porcentajes.

**Datos reales hoy:** 5 h 0% (sin ventana activa), semana 0% (reinicia el lunes 05-10), mes 3% (reinicia el 12-10), 107 M tokens en 7 días y `deepseek-v4.1-flash` con el 93%.

## 3. Identidad visual

| Token | Valor | Uso |
|---|---|---|
| `bg` | `#000000` | Fondo, negro puro en AMOLED |
| `element` | `#1e1e1e` | Pista de las barras y fondo de los chips |
| `border` | `#3c3c3c` | Borde de 1 px, esquinas rectas |
| `text` | `#eeeeee` | Valores |
| `muted` | `#808080` | Etiquetas y líneas secundarias |
| `primary` | `#fab283` | Durazno de la marca |
| `warning` | `#f5a742` | Splash: modo cerca del límite |
| `error` | `#e06c75` | Barra ≥ 85% o límite alcanzado, sin datos, desconectado |
| `success` | `#7fd88f` | Punto de sesión activa |
| Logo | `#F1ECEC` / `#B7B1B1` / `#4B4646` | Marca "o" y wordmark |

- **Fuente:** IBM Plex Mono (licencia OFL), la alternativa que declara opencode.ai. Berkeley Mono es comercial y no se puede usar.
- **Logos:** los SVG oficiales del repo `anomalyco/opencode` (licencia MIT). Su procedencia se anota en `ATTRIBUTION.md`.
- **Lenguaje visual:**
  - Paneles de borde fino con esquinas rectas.
  - Barra segmentada en bloques.
  - Chips al estilo de la terminal.
  - Todo el texto en monoespaciada.

## 4. Pantalla de uso de OpenCode

Tiene la misma estructura que la de Claude (dos paneles con barra y "Resets in"), pero con el lenguaje visual de OpenCode. Las coordenadas exactas están en el **Anexo A**.

| Elemento | Contenido |
|---|---|
| Encabezado | Marca "o" a la izquierda, wordmark centrado y batería a la derecha |
| Panel 1 | Héroe `37%` · chip `5h` · barra · `Resets in 2h 14m`. Sin ventana activa: héroe `0%` y línea `No active window` |
| Panel 2 | Héroe `22%` · chip `week` · barra · `Resets in 3d 4h` |
| Chip | Con `src = est` agrega ` · est.` En rojo si el valor es ≥ 85% o si se alcanzó el límite (`limit`) |
| Estadísticas | `7d tokens` → `107.0M` · `top model` → `ds-v4.1-flash 93%` · `month` → `3% · 14d` (porcentaje del mes y días hasta el reinicio) |
| Línea de estado | Activa: `■ build · working`, con el cuadrado verde parpadeando. Inactiva: `idle · last activity 21m ago` |
| Solo consumo | Héroes con tokens (`3.2M` / `107.0M`), chips `today` / `7 days`, sin barra y con la línea `$0.00 spent` / `$1.10 spent`. La columna de estadísticas `month` pasa a ser `7d cost` |

## 5. Splash de OpenCode

- **Escenario:** 60×60 celdas, igual que el de Clawd. La celda mide `min(W,H)/60` (8 px en 480, 6 en 368 y 4 en 240) y el fondo es negro.
- **Contenido:** solo material oficial (`research/08`): marca, wordmark, escáner de bloques de la terminal y la temporización de su cursor de escritura.

| Escena | Qué muestra | Cuándo (automático) |
|---|---|---|
| `typeon` | El wordmark se escribe letra por letra y queda un cursor durazno parpadeando (500 ms encendido, 500 ms apagado) | Inactivo (`a = 0`) |
| `scanner` | La marca "o" con el escáner de 8 bloques de la terminal (54 cuadros de 40 ms) | Activo (`a ≥ 1`) |
| `scanner` rápido | 20 ms por cuadro, sin pausa | Ocupado (`a ≥ 2`) |
| `scanner` ámbar | Escáner en `#f5a742` | Cerca del límite: 5 h o semana ≥ 75% |
| `scanner` rojo detenido | Escáner apagado y el interior de la marca parpadeando en rojo | Límite alcanzado (`st = limited`) |
| `assemble` | La marca se arma celda por celda y luego "respira" | Una vez cuando se reinicia una ventana (baja el porcentaje); después vuelve al modo automático |

PWR corto en este splash pasa a la siguiente escena, igual que hoy pasa a la siguiente animación de Clawd.

## 6. Navegación

**Ciclo:** Splash Clawd ↔ Uso Claude ↔ Splash OpenCode ↔ Uso OpenCode, circular. Si nunca llegaron datos de OpenCode, el ciclo es Splash Clawd ↔ Uso Claude, como hoy.

| Entrada | Pulsación corta (< 300 ms) | Mantener (≥ 300 ms) |
|---|---|---|
| **BOOT (lateral izquierdo)** | ◀ Pantalla anterior. En boards sin lateral derecho: ▶ siguiente | Espacio mientras se mantiene (dictado por voz de Claude Code) |
| **SECONDARY (lateral derecho)** (2.16 S3/C6, LCD-1.54) | ▶ Siguiente pantalla | Shift+Tab una vez (cambio de modo) |
| **PWR** | Splash: siguiente animación o escena. Otras pantallas: brillo | 3 s: emparejar (sin cambios) |
| **Toque en pantalla** | ▶ Siguiente pantalla, en todos los boards. En el LCD-4 es la vía principal | — |

- **Costo del cambio:** Espacio y Shift+Tab salen ~300 ms más tarde que hoy, porque hay que distinguir el toque de la pulsación mantenida.
- **Indicador de página:** 4 puntos (2 sin OpenCode) al pie, visibles 1,5 s después de cada cambio.
- **Sin cambios:** la primera pulsación después de dormir se sigue ignorando, y el emparejamiento sigue igual.

## 7. Estados de la pantalla de uso

| Estado | Condición | Cómo se ve |
|---|---|---|
| En vivo, activa | `a ≥ 1` | `■ build · working` |
| En vivo, inactiva | `a = 0` | `idle · last activity 21m ago` |
| Cerca del límite | 5 h o semana ≥ 85% | Barra y chip en `error` |
| Límite alcanzado | `st = limited` | Barra llena en rojo, chip `limit` |
| Estimado | `src = est` | Chips con ` · est.` |
| Datos viejos | Más de 5 min sin payload | Paneles al 40%, `○ stale · updated 12m ago` |
| BLE desconectado | Se cae el enlace | Paneles al 40%, `○ bluetooth disconnected` |
| Nunca recibió datos | — | Las pantallas de OpenCode no entran en el ciclo |

## 8. Protocolo BLE

Misma característica RX. El payload se distingue con `"k":"oc"`; un payload sin `k` sigue siendo de Claude. Máximo 240 bytes.

```json
{"k":"oc","ok":true,"src":"api","p5":37,"r5":134,"pw":22,"rw":5040,"pm":3,"rm":20354,"st":"ok","t7":106969,"m":"ds-v4.1-flash","ms":93,"a":1,"ag":"build","la":35}
```

| Clave | Significado |
|---|---|
| `src` | `api`, `est` o `none` (solo consumo) |
| `p5`, `pw`, `pm` | Porcentajes de 5 h, semana y mes |
| `r5`, `rw`, `rm` | Minutos hasta cada reinicio. `r5 = -1` significa que no hay ventana activa |
| `st` | `ok` o `limited` |
| `t7` | Tokens de 7 días, en miles |
| `m`, `ms` | Modelo principal, en ASCII de hasta 14 caracteres, y su porcentaje |
| `a`, `ag`, `la` | Sesiones activas, agente de la más reciente y segundos desde la última actividad |
| Solo con `src = none` | `tk` y `cd` (tokens en miles y USD de hoy) y `c7` (USD de 7 días) |

**Firmware:**
- Enruta el payload por `k` antes de `parse_json`.
- Responde ack/nack según el parseo de OpenCode.
- Usa un buffer RX de 2 posiciones.
- La frescura de OpenCode (`OC_FRESH_MS = 300000`) es independiente de la de Claude.

**Daemon:** manda el payload de OpenCode al menos 250 ms después del de Claude, y solo si la configuración dice `opencode = on`.

## 9. Daemon (macOS primero)

- Módulo nuevo `daemon/opencode_collector.py`, que se ejecuta cada 60 s dentro de `asyncio.to_thread`:
  1. Lee la clave de `auth.json` y llama al endpoint (URL con barra final, timeout de 10 s). Si falla, usa la estimación local de §2.2.
  2. Lee `opencode.db` en solo lectura (`?mode=ro`, compatible con WAL) para la actividad de §2.3.
- Funciona aunque Claude no tenga token.
- **Nunca** registra la clave ni la cabecera `Authorization`.
- Configuración en `~/.config/claude-usage-monitor/config`: `opencode = on|off`, por defecto `off`. Opcionales: `opencode_db` y `opencode_auth`.
- Tests: `daemon/tests/test_opencode_collector.py`, con una base SQLite de prueba y el endpoint simulado.
- Linux y Windows quedan para después.

## 10. Firmware (resumen, se implementa después de aprobar)

- **Estructuras y pantallas:**
  - `data.h`: nuevo `OcData`.
  - `ui.h` / `ui.cpp`: `SCREEN_OC_SPLASH` y `SCREEN_OC_USAGE`, `init_opencode_screen()`, `ui_update_opencode()` y un layout para los 3 tamaños (Anexo A).
  - Navegación con `ui_next_screen()` / `ui_prev_screen()` y el indicador de página.
- **Splash de OpenCode:** `oc_splash.cpp`, procedural porque son pocas celdas y no necesita frames pregenerados. Reutiliza el canvas del splash y su geometría (en C6, el buffer chico escalado de `splash_geometry`).
- **Entrada y botones:**
  - `main.cpp`: enrutamiento por `k`, y la distinción entre toque y pulsación mantenida en BOOT y SECONDARY. El HID se retrasa a 300 ms.
  - `ble.cpp`: buffer RX de 2 posiciones.
- **Fuentes nuevas:** IBM Plex Mono en ASCII (48/40/24/18/16/14/12), unos 60–80 KB de flash. **Hay que medir la partición de app de 3,19 MB del 2.16 original.**
- **Recursos:** marca y wordmark en RGB565A8 (unos 12 KB).
- **Simulador:** `firmware/sim/scenario-opencode.jsonl`.
- **Documentación:** `CLAUDE.md`, `README.md` y `ATTRIBUTION.md`.

---

## Anexo A: coordenadas de la pantalla de uso

Todo va en px. `top` es el borde superior del texto, con `line-height: 1`. Las posiciones dentro de un panel son relativas al panel. `seg` = ancho × alto + separación.

| Elemento | Grande 480×480 | Compacto 368×448 | Chico 240×240 |
|---|---|---|---|
| Marca "o" (x,y,w×h) | 20,24 · 40×50 | 20,22 · 32×40 | 8,6 · 16×20 |
| Wordmark (x,y,w) | 140,32 · 200 | 109,30 · 150 | 70,10 · 100 |
| Batería (x,y,w×h) | 412,34 · 40×20 | 316,30 · 32×16 | 208,10 · 24×12 |
| Panel 1 (x,y,w×h) | 20,96 · 440×132 | 20,80 · 328×116 | 8,34 · 224×82 |
| Panel 2 (x,y,w×h) | 20,240 · 440×132 | 20,206 · 328×116 | 8,122 · 224×82 |
| Padding del panel | 16 | 14 | 8 |
| Héroe (top, tamaño) | 12 · 48 | 10 · 40 | 6 · 24 |
| Chip (top, alto, fuente, padding-x) | 18 · 26 · 18 · 8 | 14 · 22 · 16 · 6 | 8 · 16 · 12 · 5 |
| Barra (top, segmentos, seg) | 72 · 34 · 10×20+2 | 60 · 25 · 10×16+2 | 38 · 18 · 9×10+2 |
| Línea de reinicio o costo (top, tamaño) | 104 · 18 | 92 · 16 | 58 · 12 |
| Estadísticas: títulos / valores (top) | 386 / 410 | 334 / 356 | — |
| Estadísticas: columnas x, tamaño | 20, 150, 370 · 18 | 20, 120, 276 · 16 | — |
| Recorte del modelo | 12 caracteres + … | 9 caracteres + … | — |
| Estado (top, tamaño, cuadrado; x del cuadrado / x del texto) | 448 · 18 · 10; 20 / 38 | 404 · 16 · 8; 20 / 34 | 214 · 12 · 6; 8 / 18 |
| Indicador de página (centro y) | 472 | 440 | 234 |
