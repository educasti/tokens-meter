# Traducción al español — glosario y reglas

Fuente de verdad para la traducción de las pantallas del dispositivo al español.
Todos los subagentes deben respetar este glosario para que la terminología sea
consistente entre pantallas.

## Conjunto de caracteres permitidos

Los fonts se regeneraron (`tools/regen_fonts_spanish.sh`) para cubrir ASCII
(0x20-0x7E) más exactamente estos glifos:

```
á é í ó ú ü ñ   Á É Í Ó Ú Ü Ñ   ¡ ¿
```

- **No uses ningún otro carácter no ASCII.** Nada de `«»`, ni comillas
  tipográficas, ni guion largo (—).
- `·` (U+00B7) y `…` (U+2026) viven SOLO en los cortes mono y plex, NUNCA en
  styrene ni tiempos: un `·` en styrene dibuja el recuadro de sustitución de
  LVGL. No los uses en títulos (tiempos), porcentajes/píldoras/líneas de
  estado (styrene) ni códigos de emparejamiento; solo donde la etiqueta use
  un font mono o plex que los cubra.
- Los comentarios del código sí pueden usar cualquier carácter (no se renderizan
  en el dispositivo), pero mantén el mismo estilo.

> Excepciones ya aprobadas (no "retraducir"): `Pairing code`→`Código`
> (`Código de emparejamiento` medía 684.6px en tiempos_56 y no cabía);
> ritmo enterprise `Ritmo bajo`/`En ritmo`/`Ritmo alto`;
> `Portfolio`→`Portafolio`; `Update failed`→`No se actualizó`.

## Reglas de traducción

1. **Español neutro** (ni de España ni de Latinoamérica salvo fuerza mayor).
   Tuteo: "tu presupuesto", no "su presupuesto".
2. **Nombres de marca no se traducen**: Claude, OpenCode, Clawd, AM/PM.
3. **Cadenas cortas**: el panel es de 480×480 y las etiquetas tienen anchos
   ajustados. Si la traducción obvia es más larga que el original, acórtala
   ("Se renueva en 42 min", no "Se restablecerá dentro de 42 minutos").
4. **Los identificadores de código, claves JSON y comentarios de protocolo
   siguen en inglés.** Solo se traduce el texto visible al usuario y los
   comentarios explicativos.
5. **No cambies la lógica ni el layout**: mismo número de etiquetas, mismas
   posiciones, mismos buffers `snprintf` (si hace falta más espacio, amplía el
   buffer, no recortes la traducción).
6. **Verifica el ancho** con `tools/check_label_widths.py` (lo ejecuta el
   agente de verificación): ninguna etiqueta debe desbordar su contenedor.

## Glosario (EN → ES)

### Pantalla de uso de Claude (`ui.cpp`)

| Inglés | Español |
|---|---|
| Usage (título) | Consumo |
| Current (etiqueta de sesión) | Actual |
| Weekly | Semanal |
| Spending (enterprise) | Gasto |
| Period (enterprise) | Periodo |
| of your monthly budget | de tu presupuesto mensual |
| Resets in %dm | Se renueva en %d min |
| Resets in %dh %dm | Se renueva en %d h %d min |
| Resets in %dd %dh | Se renueva en %d d %d h |
| Resets %s (fecha, enterprise) | Renueva %s |
| Under pace | Ritmo bajo |
| On pace | En ritmo |
| Over pace | Ritmo alto |
| To pair | Para emparejar |
| hold the power button | mantén pulsado el botón |
| for 3 seconds, then release | central durante 3 s y suéltalo |
| Pairing code | Código (excepción aprobada: `Código de emparejamiento` medía 684.6px en tiempos_56 y no cabía) |
| Enter this code in the owner tool to pair | Introduce este código en la herramienta para vincular |
| Can't reach the server - retrying | Sin respuesta del servidor - reintentando (excepción aprobada: ui.cpp:670 usa `-` y no `·` porque U+00B7 no existe en styrene/tiempos) |
| Pairing (estado) | Emparejando |
| Waiting | Esperando |
| No data | Sin datos |
| Listening | Escuchando |
| Connected | Conectado |
| AM / PM | AM / PM (sin cambios) |

### Línea de estado OTA (`ota.cpp`, `ota_pull.cpp`)

| Inglés | Español |
|---|---|
| Checking | Comprobando |
| Updating | Actualizando |
| Verifying | Verificando |
| Restarting | Reiniciando |
| Update %s | Actualización %s |
| Update failed | No se actualizó |
| OTA… | OTA… (sin cambios) |

### Pantalla de OpenCode (`ui_opencode.cpp`)

| Inglés | Español |
|---|---|
| No active window | Sin ventana activa |
| Resets in %s | Se renueva en %s |
| 7d tokens | Tokens 7 d |
| top model | Modelo principal |
| 7d cost | Gasto 7 d |
| month | mes |
| 5h (chip) | 5 h |
| week (chip) | semana |
| est. | est. (sin cambios) |
| stale · updated %s ago | desactualizado · hace %s |
| 35s / 21m / 3h 4m | 35 s / 21 min / 3 h 4 min |

### Pantalla de portfolio (`ui_portfolio.cpp`) — ya traducida, no reinventar

`Portfolio`→`Portafolio` (excepción aprobada). Cadenas:

`Cambio hoy` · `Cambio %` · `Valor total` · `Mayores variaciones` ·
`Sin cambios por ahora` · `COP`

### Splash (`splash.cpp`, `oc_splash.cpp`)

El placeholder de desarrollo "no animations loaded / run tools/…" se traduce
como "sin animaciones cargadas / ejecuta tools/…". El resto de las pantallas
splash son gráficos (sin texto), salvo que aparezca texto nuevo.

### Verbos del estatus animado (`ui.cpp` `anim_messages[]`)

Traduce manteniendo el tono fantasioso y en minúscula (el código les añade la
mayúscula inicial al renderizar). Si un juego de palabras no tiene equivalente,
inventa uno con la misma gracia en lugar de dejarlo en inglés:

`Clauding` → `Claudeando` · `Booping` → `Boopeando` ·
`Flibbertigibbeting` → `Paroleando` · `Reticulating` → `Reticulando` ·
`Schlepping` → `Arrastrando` · `Puttering` → `Pululando`
