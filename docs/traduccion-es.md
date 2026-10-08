# Traducción al español de las pantallas del dispositivo

Todas las pantallas del Clawdmeter hablan español neutro (con acentos): la
pantalla de consumo de Claude, las dos de OpenCode, la de portafolio BVC, los
avisos de emparejamiento, la línea de estado y los estados OTA. También los
scripts de instalación del host (`install.sh`, `install-mac.sh`,
`install-windows.ps1`, `flash.sh`, `flash-mac.sh`, `screenshot.sh`).

Referencia de estilo: `docs/glosario-es.md` (glosario EN→ES obligatorio y
juego de caracteres permitido).

## Fonts con acentos

Los `firmware/src/font_*.c` son bitmaps precompilados que solo cubrían ASCII
(0x20-0x7E). Se regeneraron con 16 glifos más
(`á é í ó ú ü ñ Á É Í Ó Ú Ü Ñ ¡ ¿`, +358 KB de flash total):

```bash
tools/regen_fonts_spanish.sh   # regenera los 17 con lv_font_conv@1.5.3 + parche LVGL 9
```

El parche LVGL 9 es repetible (`tools/patch_lvgl_font.py`): regenerar con el
rango antiguo reproduce los archivos anteriores byte a byte. Receta original
en `docs/fonts.md`.

Límite conocido: `·` (U+00B7) y `…` (U+2026) viven **solo** en los cortes mono
y plex, nunca en styrene ni tiempos. Donde la etiqueta usa styrene/tiempos se
usa `-` como separador (ver excepciones).

## Excepciones al glosario (por ancho de pantalla, 480 px)

- `Pairing code` → `Código` (`684.6 px` en tiempos_56 no cabía; la pista de
  abajo ya explica la acción).
- Ritmo enterprise → `Ritmo bajo` / `En ritmo` / `Ritmo alto` (la línea
  recolorada completa debe caber en ~408 px).
- `Portfolio` → `Portafolio` (uso BVC).
- `Update failed` → `No se actualizó` (cabe en el presupuesto de 340 px de la
  línea OTA; `Error al actualizar` no).
- `Sin respuesta del servidor - reintentando` usa `-`, no `·` (ver límite).
- `bluetooth desconectado` → `Bluetooth desconectado` (marca, coherente con
  portfolio); `! %d aviso, ver log` → `ver registro`.

Buffers ampliados (el español alarga ~40 % las líneas; no se recortó texto):
`ui.cpp` `buf[48]→[96]`, `last_hint[48]→[64]`; `ui_opencode.cpp`
`fmt_mins`/`fmt_age` `12→16`, `draw_status`/`status_text` `48→64`;
`ota_pull.cpp` `line[OTA_VERSION_MAX+8]→[+16]`.

## Verificación

```bash
tools/verify_spanish.sh          # charset + anchos + build waveshare_amoled_216
python3 -m pytest daemon/tests -q  # 277 passed (requiere bleak, httpx, Pillow)
```

- `tools/check_label_widths.py` decodifica las métricas reales de cada font y
  mide cada cadena en píxeles (`--width`, `--audit`, `--charset`, `--max`).
  Nota: `--audit` no siempre resuelve el font de la etiqueta (`(sin
  resolver)` = cota con todos los fonts del archivo, sobreestima); confirmar
  con `--width <font real>`.
- Build S3 verificado: RAM 23 %, Flash 50.3 %.
- Protocolo intacto: claves JSON, comandos `{"cmd":…}`, `WRITE_ENC`,
  `name="ssid"/"pass"`, SSIDs y credenciales sin tocar. Trazas `Serial` en
  inglés (diagnóstico, no UI).

## Portal cautivo

El portal (`firmware/src/portal.cpp`) vive en `main` con página en español y
escaneo de redes WiFi (commit `dcf259f`). Esta rama no lo toca: ante cualquier
solape, la versión de `main` manda para no regresar el escaneo.
