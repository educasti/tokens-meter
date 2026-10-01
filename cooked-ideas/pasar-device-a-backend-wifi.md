# Pasar el device de "BLE + Mac local" a un backend en la nube por WiFi, con auth por device, revocación remota y provisioning de WiFi asistido por el celular

Started: 2026-10-01
Status: done

## Angles to cover
- [x] 1. Qué se va y qué se queda (¿el Mac sale del datapath o solo cambia el transporte?)
- [x] 2. Dónde vive el backend (VPS público vs. casa + túnel)
- [x] 3. Qué datos salen de tu Mac (privacidad / modelo de confianza)
- [x] 4. Autenticación del dispositivo (token / mTLS / pairing code)
- [x] 5. Revocación y pérdida del dispositivo
- [x] 6. Provisioning de WiFi con el celular
- [x] 7. Push vs. polling y batería
- [x] 8. Coste, operación y fallback
- [x] 9. **(añadido mid-interview)** Camino Mac → VM: cómo se autentica el Mac y cómo se emiten los tokens

## Interview

### 1. Qué se va y qué se queda
**Q:** ¿El Mac sigue colectando y publicando (y solo desaparece la cercanía/BLE), o sale del datapath del todo?
**Claude's lean:** El Mac se queda como **colector**; cambia el **transporte** (BLE → publicar en backend y que el device baje por WiFi). BLE se conserva para provisioning, disparo de OTA y fallback sin WiFi. Sacar al Mac del datapath obligaría a que alguien más tenga las credenciales de Claude/OpenCode, que es la opción ya descartada.
**Answer:** Aceptado. El Mac se queda como colector; lo que se va es la dependencia de cercanía/BLE. BLE queda para provisioning, OTA y fallback.

### 2. Dónde vive el backend
**Q:** ¿VPS público propio (A), managed/serverless (B), o casa + túnel (C)?
**Claude's lean:** B (serverless) para evitar operar un servidor; A si se quiere evitar vendor-lock.
**Answer:** Ya tiene una **VM de Oracle Cloud 24/7 conectada a internet** → es exactamente la opción A, y mejor que contratar una nueva: propia, gratis, siempre arriba. Esa VM es el backend. Además plantea que la VM podría ser también el colector.
**Nota del grill:** el rol de "colector" de la VM se difiere al ángulo 3, porque implica mover (o no) el token de Claude a la nube. Se confirma aquí solo el **backend = VM Oracle Cloud**.

### 3. Qué datos salen de tu Mac
**Q:** ¿El token de Claude vive en la VM (device con datos aunque el Mac esté apagado) o se queda en el Mac (la VM solo guarda números)?
**Claude's lean:** El token se queda en el Mac. La VM guarda **solo números** `s, sr, w, wr`. Si el Mac está offline, el device muestra el último valor con marca "stale". Colectar Claude en la VM ampliaría la superficie de ataque a cambio de frescura solo cuando el Mac está apagado.
**Answer:** Aceptado. Token solo en el Mac; la VM almacena únicamente números de usage; el device usa el último valor con etiqueta de stale cuando el Mac está caído.

### 4. Autenticación del dispositivo
**Q:** ¿Bearer token por device o mTLS?
**Claude's lean:** Bearer token por device (32 bytes aleatorios, en NVS, `Authorization: Bearer`, TLS con CA raíz). mTLS es más fuerte pero obliga a gestionar certificados/CRL/OCSP y gasta RAM en el handshake, para un caso de un dueño con uno o pocos devices.
**Answer:** Aceptado. Bearer token único por device; sin mTLS.

### 5. Revocación y pérdida del dispositivo
**Q:** ¿Basta con revocar server-side, o se diseña además borrado remoto?
**Claude's lean:** La revocación autoritativa es server-side (revocar el token → 401). El wipe remoto es defensa en profundidad y best-effort (un device perdido puede no reconectar nunca), no el control principal. Un finder solo ve los últimos números en pantalla, no el token.
**Answer:** Aceptado. Revocación server-side como control principal + wipe remoto best-effort; cerrar el bond BLE en el Mac al perderlo.

### 6. Provisioning de WiFi con el celular
**Q:** ¿SoftAP + portal cautivo (sin app) o BLE desde una app?
**Claude's lean:** SoftAP + portal cautivo. Es la única vía cómoda en iOS y Android sin instalar nada (iOS no tiene Web Bluetooth). El device levanta su AP, el celular entra al portal y escribe SSID/clave; se guardan en NVS. El popup automático es poco fiable en iOS → hay que ofrecer `http://192.168.4.1` a mano. El token se emite vía código corto de emparejamiento mostrado en pantalla.
**Answer:** Aceptado. SoftAP + portal cautivo, disparado cuando no hay credenciales; token por código de emparejamiento.

### 7. Push vs. polling y batería
**Q:** ¿Poll por timer con radio apagada entre fetches, o conexión persistente (MQTT/WS)?
**Claude's lean:** Poll por timer: WiFi encendido ~2–3 s por ciclo, apagado el resto; intervalo ~5 min; fetch inmediato al tocar la pantalla o pulsar un botón; intervalo adaptativo según si está cargando. Push mantiene la radio asociada y drena la batería, se deja como optimización futura.
**Answer:** Aceptado. Polling con radio apagada entre fetches + wake por interacción + adaptativo por fuente de energía. Se reemplaza la ventana de `stale` de 90 s por una alineada al intervalo.

### 8. Coste, operación y fallback
**Q:** ¿Backend mínimo self-hosted en la VM, o gestionado?
**Claude's lean:** Mínimo self-hosted: servicio chico + SQLite con tokens hasheados, Caddy + Let's Encrypt (hace falta hostname, no IP pelada). Único coste opcional: el dominio. Riesgo a conocer: Oracle reclama instancias Always Free inactivas. Fallback: sin WiFi o VM caída → último valor + `stale`; la OTA y el provisioning no dependen de la VM.
**Answer:** Aceptado. Backend mínimo self-hosted en la VM Oracle; fallback por último valor + stale; OTA/provisioning independientes de la VM.

## New angle surfaced
Al cerrar el ángulo 8 quedó claro que el camino **Mac → VM** (cómo publica el Mac y con qué credencial) no estaba cubierto por ningún ángulo. Se añade como **ángulo 9** y se cubre antes de cerrar.

## Summary

Cerrada 2026-10-01. 9 ángulos cubiertos (8 previstos + 1 añadido al vuelo).

**Recap y dónde quedó cada ángulo**

1. **Qué se va / qué se queda** — El Mac sigue siendo el colector; lo que desaparece es la dependencia de cercanía/BLE. BLE se conserva para provisioning, OTA y fallback.
2. **Dónde vive el backend** — Tu **VM de Oracle Cloud** (opción A), ya 24/7 y propia. Se descartó casa + túnel porque el colector viaja.
3. **Qué sale del Mac** — Solo **números** (`s, sr, w, wr`). El token de Claude se queda en el Mac; sin token en la nube.
4. **Auth del device** — **Bearer token por device** (32 B, en NVS, header `Authorization`, TLS con CA raíz). Sin mTLS.
5. **Revocación / pérdida** — Control autoritativo **server-side** (revocar token → 401); wipe remoto **best-effort**; cerrar el bond BLE en el Mac.
6. **Provisioning WiFi** — **SoftAP + portal cautivo** (sin app, compatible iOS/Android); token por **código corto de emparejamiento**.
7. **Push vs polling** — **Polling por timer** con radio apagada entre fetches, wake por interacción, intervalo adaptativo por fuente; se alarga la ventana de `stale`.
8. **Coste / operación / fallback** — Backend **mínimo self-hosted** (servicio chico + SQLite con tokens hasheados + Caddy/Let's Encrypt). Fallback: último valor + `stale`; OTA y provisioning no dependen de la VM.
9. **Camino Mac → VM** — El Mac **empuja** con una **API key de usuario** (solo escribe); el device **lee** con su token (solo lee). Privilegio mínimo, sin credencial única todopoderosa.

**Recomendación de Claude**

La arquitectura es coherente y cada pieza quedó con una elección defendible. Mi posición: **adelante, pero construida como una rebanada vertical delgada y no como todo el sistema de golpe.**

El riesgo principal de esta decisión **no es técnico, es operativo**: conviertes un widget local trivial (BLE + daemon) en un **sistema distribuido que ahora tienes que operar** — VM, TLS/dominio, auth, revocación, provisioning, OTA, dos credenciales y polling consciente de batería. Nada de eso es difícil por separado, pero en conjunto **se oxida si no lo tocas en meses** (expira el cert, Oracle reclama la instancia, cambia la IP, rota un token).

Por eso el orden que recomiendo:

1. **Primero el tubo:** Mac hace `POST /usage` con API key → la VM guarda → el device hace `GET /usage` con bearer token. Sin provisioning bonito, sin OTA nuevo, sin optimización de batería.
2. **Después el ciclo de vida:** código de emparejamiento + emisión de token + revocación.
3. **Después la comodidad:** SoftAP + portal cautivo.
4. **Al final la batería:** polling adaptativo y medición real.

Si la rebanada 1 no te resulta útil, no tiene sentido invertir en las 3 restantes.

**Preguntas abiertas / información que falta**

- **IP pública de la VM:** ¿es efímera o reservada? En Oracle las IPs efímeras cambian al detener/arrancar la instancia, y eso rompe el device y el TLS. Conviene reservar una IP pública fija.
- **Hostname para Let's Encrypt:** ¿tienes un dominio, o usas DuckDNS? Let's Encrypt no emite para IP pelada de forma cómoda.
- **Red de Oracle:** además de la Security List, las imágenes traen `iptables` que bloquea por defecto; hay que abrir 443 en ambas capas.
- **Reloj del device para TLS:** el ESP32 necesita hora correcta (SNTP) antes de validar el certificado. Sin esto, la validación TLS falla. Es un detalle de implementación obligatorio.
- **Reclaim de Oracle Always Free:** confirmar el tipo de instancia y si aplica la política de reclamo por inactividad.
- **Retención en la VM:** ¿guardas solo el último valor o histórico? Afecta el tamaño de la DB y los backups.
- **OpenCode y portfolio en la nube:** hoy salen de archivos locales del Mac. ¿También quieres esas pantallas en modo standalone (implica publicar esos datos), o solo Claude?
- **Presupuesto de batería:** medir la autonomía real con el intervalo elegido antes de fijarlo.

