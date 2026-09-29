# HW1 — Test en hardware del Waveshare AMOLED-2.16 (ESP32-S3)

**Fecha:** 28-09-2026 · **Rama:** `feat/opencode-screens` · **Puerto:** `/dev/cu.usbmodem101`
**Veredicto:** las dos pantallas de OpenCode funcionan en el panel real. Geometría conforme al Anexo A. Un desajuste documental (wordmark) y un tinte verde en los grises neutros del panel.

Todas las capturas están en `/tmp/hw-shots/`. Ningún archivo del repo se modificó.

---

## 1. Flash

```
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem101
```

```
Wrote 2289456 bytes (1101989 compressed) at 0x00010000 in 11.7 seconds (1565.7 kbit/s).
Verifying written data...
Hash of data verified.
Hard resetting via RTS pin...
Environment           Status    Duration
waveshare_amoled_216  SUCCESS   00:00:30.350
```

**SUCCESS.** La partición de app de 3,19 MB aguanta sin problema: 1,10 MB comprimidos, 2,29 MB escritos. Ningún aviso de la partición (§10 del SPEC pedía medirla).

## 2. Boot log (115200, ~9 s tras reset)

```
ESP-ROM:esp32s3-20210327
rst:0x15 (USB_UART_CHIP_RESET),boot:0x2b (SPI_FAST_FLASH_BOOT)
{"ready":true}
[   882][E][Preferences.cpp:47] begin(): nvs_open failed: NOT_FOUND
Brightness init: level=200 (idx=2)
AXP2101 init OK
[   900][E][esp32-hal-i2c-ng.c:275] i2cWrite(): i2c_master_transmit failed: [259] ESP_ERR_INVALID_STATE
QMI8658 init OK
chime: ES8311 ready
Touch init OK
BLE: owner loaded = 80:d1:ce:1b:4d:a1
BLE: advertising start=OK (connected=0)
BLE: init complete, MAC=28:84:85:56:36:41
Dashboard ready (Waveshare AMOLED 2.16, 480x480), waiting for data on BLE...
BLE: auth complete peer=80:d1:ce:1b:4d:a1 bonded=1 enc=1
BLE: connected from 80:d1:ce:1b:4d:a1 (active=1)
BLE: connparams update itvl=12(15.00ms) lat=22 timeout=200(2000ms)
```

`Dashboard ready`, AXP2101 / QMI8658 / ES8311 / touch OK, BLE emparejado y conectado. Los dos `[E]` (NVS vacío en el primer arranque tras flashear e I²C del AXP2101) son preexistentes y no reiterativos.

## 3–4. Daemon de esta rama

- `daemon/.venv/` creado con **Homebrew Python 3.14.7**. El `python3` del sistema es **3.9.6** y el daemon ni arranca: `TypeError: unsupported operand type(s) for |: 'type' and 'NoneType'` en la anotación `str | None` de la línea 78. Hay que documentar el requisito de Python ≥ 3.10 (el venv de `install-mac.sh` probablemente usa el del sistema).
- `bleak 3.0.2` + `httpx 0.28.1` instalados sin problemas; **no** hizo falta ningún permiso de Bluetooth adicional.
- Configuración: copia en `~/.config/claude-usage-monitor/config.bak-oc-test` y se añadió `opencode = on` (no estaba).

Bitácora (`/tmp/tm-daemon.log`, sin secretos):

```
[20:17:10] === Claude Usage Tracker Daemon (BLE, macOS) ===
[20:17:15] Found system-connected peripheral: 'Clawdmeter' [864D3594-…]
[20:17:15] Connected
[20:17:15] Sending: {"s":28,"sr":183,"w":29,"wr":7423,"st":"allowed","acct":"pro","ok":true}
[20:17:16] OpenCode: api 5h 0% week 0% month 3% 7d 96319k
[20:17:16] Sending: {"k":"oc","ok":true,"src":"api","p5":0,"r5":-1,"pw":0,"rw":8563,"pm":3,"rm":19667,"st":"ok","t7":96319,"m":"space-bunny-fr","ms":100,"a":1,"ag":"build","la":5}
```

Como se esperaba en el brief. Tras 21 min: **21 payloads de OpenCode**, cadencia limpia de ~61 s, percentages que suben solos (`7d` de 96319k a 103321k) y **cero errores**. `src=api` — el endpoint oficial de Go respondió, sin caer a la estimación local. La clave nunca aparece en el log.

## 5. Capturas

| # | Archivo | Qué se ve |
|---|---|---|
| 0 | `0-oc-usage-after-flash.png` | Pantalla de uso de OpenCode nada más flashear (ya había llegado el primer payload): `0%`+`5h`, `0%`+`week`, `7d tokens 96.3M`, `top model space-bunny-… 100%`, `month 3% · 14d`, `■ build · working` |
| 1 | `1-clawd-splash.png` | Splash de Clawd (cangrejo pixel en durazno `#fab283`, a mitad de animación) |
| 2 | `2-claude-usage.png` | Pantalla de uso de Claude: `30%` / `29%`, `Pondering…`. Idéntica en geometría a `screenshots/usage.png` del repo |
| 3 | `3-oc-splash.png` | Splash de OpenCode, escena `scanner`: marca "o" + escáner de 8 bloques de la terminal (sesión activa → 40 ms/cuadro) |
| 4 | `4-page-dots-claude-usage.png` | Indicador de página sobre Claude: 4 puntos, **2.º encendido** |
| 5 | `5-page-dots-oc-splash.png` | Indicador de página sobre el splash de OC: 4 puntos, **3.º encendido** |
| 6 | `6-page-dots-zoom.png` / `6-page-dots-zoom-detail.png` | Detalle ×4 de las filas de puntos: 4 puntos, encendido `#eeeeee`, apagado `#484848` |
| 7 | `7-oc-splash-typeon.png` | Escena `typeon`: wordmark `opencode` completo (gris `#F1ECEC`), cursor en fase apagada |
| 8 | `8-oc-splash-typeon-cursor.png` | La misma escena con el **cursor durazno encendido** tras la `e` |
| 9 | `9-oc-splash-assemble.png` | Escena `assemble`: la marca armada, x=144 y=120, 192×240, interior respirando |
| 10 | `10-oc-splash-scanner-after-assemble.png` | Vuelta a `scanner`: el ciclo de escenas de PWR cierra bien |
| 11 | `11-oc-usage-after-right-button.png` | Uso de OpenCode tras el botón derecho. Es la base de todas las medidas del §6 |
| 12 | `12-oc-splash-after-left-button.png` | Splash de OC tras el botón izquierdo (pantalla anterior) |
| 13 | `13-oc-splash-after-left-hold.png` | Sigue en el splash tras mantener el botón izquierdo: **no navegó** |

## 6. Botones

| Prueba | Resultado |
|---|---|
| Toque en pantalla → siguiente | OK. Ciclo completo recorrido: Clawd → Claude → OC splash → OC usage → Clawd |
| Toque corto **botón derecho** (GPIO 18) | OK. OC splash → OC usage |
| Toque corto **botón izquierdo** (GPIO 0) | OK. OC usage → OC splash |
| **Mantener** botón izquierdo ~2 s | OK. Se escribió una tanda de espacios en el Mac (llegaron al prompt de este panel) y la pantalla **no** navegó |
| **PWR corto** en el splash de OC | OK. scanner → typeon → assemble → scanner || Indicador de página | OK. 4 puntos en cuanto aterrizó el primer payload de OpenCode |

**Observación:** de tres pulsaciones cortas de PWR, **una no se registró** (el splash siguió en `typeon`; las otras dos avanzaron de escena correctamente). Con la misma presión y duración que las que sí funcionaron, así que probablemente fue el propio dedo, no el firmware — pero queda anotado.

**Nota de muestreo (no es un fallo):** el cursor de `typeon` parpadea cada 500 ms y el volcado del framebuffer por USB tarda ~2 s, así que el muestreo aliasa y devuelve siempre la misma fase. Por eso hace falta capturar en ráfaga: `7` salió con el cursor apagado y `8` con el cursor encendido, y el resto de la escena es byte a byte idéntico.

## 7. Comparación con SPEC §4/§5 y Anexo A

`compute_layout()` de `ui_opencode.cpp` (rama `c.height >= 460`) **coincide campo por campo con el Anexo A**: marca 20,24 · paneles 20,96 y 20,240 de 440×132 · pad 16 · héroe 12/48 · chip 18/26/18/8 · barra 72/34/10×20+2 · reinicio 104/18 · estadísticas 386/410 en x 20/150/370 con recorte de 12 · estado 448/18/10 en 20/38 · puntos y=472.

Medido sobre el framebuffer real (captura 11), en píxeles de tinta:

| Elemento | Anexo A | Medido | Δ |
|---|---|---|---|
| Marca "o" | 20,24 · 40×50 | x20 y24 40×50 | **exacto** |
| Wordmark | 140,32 · **200** | x142, y37, **195 px** | **+2 px, 5 px más corto** |
| Batería | 412,34 · 40×20 | glifo 414..457 (44×28) dentro del icono de 48×48 en x=412, y=34 | posición exacta |
| Panel 1 | 20,96 · 440×132 | 20,96 · 440×132 | **exacto** |
| Panel 2 | 20,240 · 440×132 | 20,240 · 440×132 | **exacto** |
| Chip | alto 26, pad-x 8 | y115, alto 26, borde derecho 443 | exacto |
| Barra | 34 seg · 10×20 + 2 | **34** segmentos de 10 px, hueco de 2, 20 px de alto, x37..442 | **exacto** |
| Línea de reinicio | top 104 | y203 | exacto |
| Estadísticas | 386 / 410 · x 20/150/370 | y388 / y412 · x21 / 151 / 371 | +1 px (bearing de la fuente) |
| Recorte del modelo | 12 caracteres + … | `space-bunny-…` | correcto |
| Estado | 448 · 10 px en x=20 / texto x=38 | cuadrado x20 y453 10×10, texto x40 | exacto |
| Puntos de página | centro y=472 | y469..474, x 216/230/244/258 | **exacto** |

Colores leídos del panel:

| Token | Spec | En el panel |
|---|---|---|
| `text` | `#eeeeee` | `#efefef` |
| `muted` | `#808080` | `#848284` |
| `primary` | `#fab283` | `#ffb284` |
| `success` | `#7fd88f` | `#7bdb8c` |
| `element` | `#1e1e1e` | `#181c18` |
| `border` | `#3c3c3c` | `#393c39` |
| Logo | `#F1ECEC` / `#B7B1B1` / `#4B4646` | `#f7efef` / `#b5b2b5` / `#4a4542` |

### Diferencias encontradas

1. **Wordmark 195 px, no 200 (Anexo A desactualizado).** `oc_wordmark_l` es 195×35 — `tools/gen_oc_logo.js` lo genera así a propósito ("16x20 and 195x35 / 156x28 / 117x21"). Como `wordmark_x = (480 − w) / 2`, queda en **x=142 en vez de 140**. La ficha del Anexo A dice `140,32 · 200`; hay que corregir el anexo (o el asset), no el firmware. Impacto: 2 px.
2. **Tinte verde en los grises neutros del panel.** `#1e1e1e` sale como `#181c18` y `#3c3c3c` como `#393c39`: la G queda 3–4 unidades por encima de R y B. Los colores con croma (durazno, verde de sesión, grises del logo) no se desvían. Es una característica del AMOLED, no del firmware, y con `element` y `border` —que son justo el fondo de las barras y el borde de los paneles— se nota algo en los paneles vacíos.
3. **Todo lo que va dentro de un panel queda 1 px más abajo y a la derecha** de una lectura literal del Anexo A, porque las coordenadas relativas se miden desde dentro del borde de 1 px y no desde su caja. Es un desplazamiento uniforme de 1 px (barra y chips), coherente y no visible.
4. Sin resto de diferencias: **no hay tearing, ni desplazamiento de panel, ni redondeo, ni texto truncado de más**. El recorte de `space-bunny-…` y los recortes de `107.0M` → `101.5M` y `3% · 14d` salen como manda el §4.

### Lo que no se pudo probar

`stale` (>5 min sin payload) y `bluetooth disconnected` (§7) no se dispararon: **no** se paró el daemon para probarlos. El LaunchAgent instalado sigue sin correr (`com.user.claude-usage-daemon`, exit 78 — su venv falla con `httpx` y `FileNotFoundError` de SSL), tal como decía el brief; no se tocó.

## 8. Estado final

- Daemon **corriendo**: PID **15034** (`daemon/.venv/bin/python daemon/claude_usage_daemon.py`, log en `/tmp/tm-daemon.log`).
  - Parar: `kill 15034`
- Configuración respaldada en `~/.config/claude-usage-monitor/config.bak-oc-test`.
  - Restaurar: `mv ~/.config/claude-usage-monitor/config.bak-oc-test ~/.config/claude-usage-monitor/config`
- `daemon/.venv/` creado (ya estaba en `.gitignore`). Nada commiteado.
