# Prototipo completo del dispositivo (`design/device-prototype`)

Un solo archivo HTML autocontenido que reúne **toda** la funcionalidad del
Clawdmeter en un simulador interactivo: las cinco pantallas del ciclo y los
flujos que no son pantalla (portal cautivo, emparejamiento, OTA híbrido y
estados del enlace BLE). Es una versión extendida de
`design/opencode-screen/prototype.html`, cuyo código de render reutiliza.

## Cómo abrirlo

No hay build ni servidor y no hace falta instalar nada:

```sh
xdg-open design/device-prototype/prototype.html   # Linux
open design/device-prototype/prototype.html       # macOS
```

Las únicas referencias externas son relativas y por eso hay que abrir el
archivo **desde el repo**, no una copia suelta:

- `../opencode-screen/oc-splash.js` — módulo del splash de OpenCode (canvas).
- `../../screenshots/splash.gif` y `../../screenshots/usage.png` — capturas
  reales de las pantallas Clawd y Claude.
- Google Fonts (IBM Plex Mono e Inter), las mismas que ya carga el prototipo de
  referencia.

## Qué cubre

### Sección A — Simulador interactivo

Simulador de 480×480 (bisel + pantalla) con la misma paleta oscura del
prototipo de referencia. Tiene un selector de **Modo**: `Pantallas` / `Flujos`.

**Pantallas** (ciclo `clawd → claude → ocsplash → ocusage → portfolio`):

| Pantalla | Cómo se dibuja |
|---|---|
| `clawd` (splash) | `screenshots/splash.gif` real, 480×480 |
| `claude` (uso) | `screenshots/usage.png` real; se atenúa con overlay si el enlace cae o los datos están viejos |
| `ocsplash` | canvas con `oc-splash.js` (escenas `typeon`/`scanner`/`assemble`, moods idle/active/busy/near/limited) |
| `ocusage` | `renderOpenCode()` portado del prototipo (dos paneles, barras segmentadas, estadísticas, línea de estado) |
| `portfolio` | `renderPortfolio()` portado del prototipo (tarjetas BVC, modo privado, estados vacíos) |

**Flujos** (no son pantallas; pestañas dentro del escenario):

| Flujo | Qué muestra |
|---|---|
| Portal cautivo (SoftAP) | página de aprovisionamiento **en español** fiel a `design/backend-wifi/WIFI-SCAN.md` §5: auto-escaneo, `Actualizar` animado, lista con barras de señal y candado, tap-to-fill del SSID, campo de contraseña, `Guardar y conectar`, y los estados `Guardado ✓` y `Falta el nombre de la red`. Al lado, el lado dispositivo: SSID `Clawdmeter-XXXX`, código de emparejamiento y el respaldo `http://192.168.4.1`. |
| Emparejamiento | código en pantalla, `POST /api/pair/start`, `manage.py pair`, `GET /api/pair/status` (token una sola vez), guardado en NVS y revocación (PHASE2-CONTRACT). |
| OTA híbrido | disparo por BLE (`{"cmd":"ota",…}`), transferencia por WiFi (`espota`), línea de estado transitoria `■ OTA`, y rollback por `boot_tries`. |
| Enlace BLE | conectado / caído / datos viejos, con las tres líneas de estado reales de OpenCode y del portafolio, paneles al 40 %. |

**Controles**: escenarios de Claude, de OpenCode y del portafolio (normal y
privado), y los toggles *BLE conectado*, *Daemon envía OC*, *Daemon envía PF* y
*Modo privado*. Botones físicos **BOOT / PWR / SEC** con toque y mantener;
PWR cicla brillo en las pantallas de uso, pasa de escena/animación en los
splashes y alterna el modo privado en el portafolio; toque en pantalla = pantalla
siguiente; puntos de página; línea de información, payload BLE con conteo de
bytes (tope 240) y explicación de navegación.

### Sección B — Todas las variantes

Grilla con cada pantalla × escenario representativo: capturas reales de Claude
(normal/caído/viejo), los cuatro moods del splash de OpenCode, los escenarios de
uso de OpenCode, el portafolio BVC completo en **normal y privado**, y el portal
cautivo (escaneando / lista / sin redes / guardado).

### Sección C — Paleta y tipografía

Colores de OpenCode (Plex) y del portafolio BVC; espécimen de **IBM Plex Mono**
(OpenCode); espécimen de **Inter** como *sustituto* declarado de **Styrene B**
(portafolio, fuente comercial compilada en la placa); y una nota sobre el
**sistema Claude** (**Tiempos** + monoespaciada, también comercial y no
cargable, por eso las pantallas de Claude se muestran como capturas reales).

## Datos ficticios

Todos los números de OpenCode y del portafolio son inventados, igual que los
SSID, direcciones y códigos del portal. La única información real es lo que el
dispositivo muestra cuando el daemon la envía en vivo. Las capturas de Clawd y
Claude sí son reales. El prototipo lo advierte en un aviso al inicio.

## Notas

- Una sola cosa dinámica usa `textContent` de forma obligatoria: las cadenas de
  SSID/red del portal (nunca `innerHTML`), tal como exige el contrato.
- Si un SSID/red del mock cambia de comportamiento, hay que mantenerlo alineado
  con `design/backend-wifi/WIFI-SCAN.md` §5: si el prototipo y esa especificación
  discrepan, manda la especificación.
- Los textos de UI están en español, igual que el prototipo de referencia.
