// OTA automático por consulta — motor de actualización HTTPS iniciado por el
// dispositivo.
//
// Contrato congelado: design/ota-pull/DESIGN.md sección 7 (máquina de estados) y
// design/ota-pull/IMPL.md sección 2.3. Módulo solo de hardware: el simulador
// nativo enlaza boards/sim/ota_sim.cpp en su lugar (el build_src_filter del
// entorno de simulación excluye este archivo), igual que con ota.cpp.
//
// Contrato de concurrencia:
//   * ota_pull_handle_ctrl() corre en la tarea de host de NimBLE y solo analiza /
//     encola una petición de una sola ranura; nunca toca WiFi ni la flash.
//   * ota_pull_tick() corre en la tarea de bucle de Arduino y no bloquea: aplica
//     la planificación de arranque / 24 h, arbitra la radio con la ruta híbrida y
//     despierta al worker.
//   * la secuencia bloqueante JOIN → … → REBOOT corre en el worker de red
//     compartido (net_worker.cpp: una pila de 12 KB de RAM interna, núcleo 0).
//     Es la dueña de la única pila larga del camino de consulta, así que la tarea
//     de bucle nunca se engorda por un saludo TLS, y la pila se comparte con los
//     workers de consulta de consumo y de emparejamiento en lugar de que cada
//     módulo mantenga residente su propia pila de 12 KB (lo que había dejado sin
///     memoria al montículo interno y roto el saludo TLS del manifiesto).
//
// Invariantes anti-brick (§7.2, §11):
//   * nunca escribir en la ranura en ejecución — se comprueba que el destino es
//     distinto de la que corre;
//   * abortar antes de activar ante cualquier fallo de hash / imagen / firma;
//   * activate (esp_ota_set_boot_partition) es el único paso de confirmación y
//     corre solo después de que el SHA-256 del manifiesto, la firma separada
//     (cuando hay una clave de firma anclada; SIGNING.md §7) y esp_ota_end()
//     pasen todos.

#include "ota_pull.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <string.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>

#include "ota.h"
#include "ui.h"
#include "ble.h"
#include "ota_manifest.h"
#include "ota_semver.h"
#include "ota_sig.h"
#include "ota_wifi.h"
#include "net_worker.h"
#include "hal/board_caps.h"
#include "hal/power_hal.h"
#include "certs/pinned_server_pem.h"
#include "certs/signing_pubkey_pem.h"

// ---- Modelo de confianza TLS (DESIGN §8) -----------------------------------
// El origen de la actualización es una IP pública pelada, así que no hay nombre
// DNS al que encadenar hacia Let's Encrypt: el firmware ancla el certificado
// AUTOFIRMADO del servidor en lugar de las raíces ISRG. `PINNED_SERVER_PEM` se
// genera en tiempo de compilación a partir del `certs/pinned_server.pem` sin
// versionar (scripts/gen_pinned_cert.py) y se pasa a
// WiFiClientSecure::setCACert() tanto para la descarga del manifiesto como para
// la del binario. La verificación está siempre ACTIVA:
//   * nunca se llama a `setInsecure()`, y la ranura de CA nunca queda en NULL,
//     así que `ssl_client.cpp` configura MBEDTLS_SSL_VERIFY_REQUIRED;
//   * un certificado ausente da como resultado un marcador que no se puede
//     analizar -> el análisis de la CA falla y el saludo se cierra de forma
//     segura en lugar de confiar en nada.
//
// Comprobación de nombre de host frente a un SAN de dirección IP: mbedTLS 3.6.x
// (arduino-esp32 3.3.8) SÍ verifica los Subject Alternative Name de tipo
// IPAddress. Solo funciona cuando la cadena de host TLS es el literal IP pelado
// (`x509_crt_check_san_ip()` pasa el host por inet_pton y lo compara byte a byte
// con el SAN). HTTPClient alimenta el host de la URL a `set_hostname()`, así que
// el requisito es:
//   * `-DOTA_PULL_MANIFEST_URL` debe ser `https://<el mismo literal IP>/...` --
//     byte a byte la dirección del SAN del certificado, sin nombre DNS, sin punto
//     final, IPv6 sin corchetes de URL;
//   * como un certificado que lleva un subjectAltName se compara SOLO contra el
//     SAN (el CN se ignora), la IP DEBE estar en el SAN, no solo en el CN.
// Si una cadena de herramientas futura deja de admitir SAN de IP, NO relajes la
// verificación: añade un nombre DNS al certificado como SAN dNSName y apunta la
// URL a ese nombre. El certificado anclado es el ancla de confianza en cualquier
// caso.

// URL base del directorio estático de firmware (DESIGN §6.4, §14.2). El host de
// la VM real se inyecta por compilación mediante
// -DOTA_PULL_MANIFEST_URL="https://<host>/firmware"; el valor de reserva es el
// TLD .invalid reservado y no enrutable, para que nunca se cuele en el código un
// host real.
#ifndef OTA_PULL_MANIFEST_URL
#define OTA_PULL_MANIFEST_URL "https://ota.invalid/firmware"
#endif

// ---- Ajustes (DESIGN §7.1, §7.3, §10) ---------------------------------------
// El worker bloqueante es la tarea compartida net_worker (una pila de 12 KB de
// RAM interna, núcleo 0, creada una sola vez en setup); este módulo solo le
// envía trabajos.
#define WIFI_JOIN_MS         20000u    // §7.1 tiempo límite de WIFI_JOIN
#define SNTP_TIMEOUT_MS      15000u    // §7.1 tiempo límite de SNTP_TIME_SYNC
#define MANIFEST_TIMEOUT_MS  15000u    // §7.1 tiempo límite de FETCH_MANIFEST
#define DOWNLOAD_IDLE_MS     15000u    // §7.1 tiempo límite de lectura parada en DOWNLOAD
#define DOWNLOAD_TOTAL_MS    300000u   // §7.1 tiempo límite total de DOWNLOAD
#define REBOOT_DELAY_MS      300u

#define HEALTHY_MS           60000u    // igual que en ota.cpp: primero se confirma el arranque
#define CHECK_INTERVAL_S     (24u * 60u * 60u)   // cadencia de 24 h (§10.1)
#define MIN_VALID_EPOCH      1700000000L         // ≈2023-11; suelo de reloj sensato (§8.2)

#define CHECK_FLOOR_PCT      20        // §10.2 suelo para comprobar
#define APPLY_FLOOR_PCT      50        // §10.2 suelo para aplicar sin estar cargando
#define BATTERY_DEFER_S      1800      // espera tras rechazar por la puerta de batería

// ---- NVS (espacio de nombres "otah", compartido con la ruta híbrida; DESIGN §14.1) ------
#define PULL_NS        "otah"
#define K_SSID         "ssid"
#define K_PASS         "pass"
#define K_AUTO         "auto"
#define K_LAST_CHK     "last_chk"
#define K_DEFER        "defer"
#define K_CHK_FAIL     "chk_fail"
#define K_PEND_VER     "pend_ver"
#define K_ETAG         "etag"
#define ETAG_MAX       40

// ---- Calendarios de reintentos (DESIGN §7.1 / §7.3) ------------------------
static const uint32_t kJoinBackoffMs[]     = { 5000, 15000, 45000 };  // tras los intentos 1,2
static const uint32_t kManifestBackoffMs[] = { 2000, 4000, 8000 };    // tras los intentos 1,2
static const uint32_t kCheckBackoffS[]     = { 5, 15, 45, 120, 600 }; // por comprobación fallida

// ---- Estado ----------------------------------------------------------------
enum pull_state_t {
    PS_IDLE,
    PS_JOIN,
    PS_SNTP,
    PS_MANIFEST,
    PS_COMPARE,
    PS_DOWNLOAD,
    PS_VERIFY,
    PS_ACTIVATE,
    PS_REBOOT,
};

// Una petición de comprobación/actualización en cola. `apply == false` es
// `mode:"check"`.
struct pull_req_t {
    bool apply;
    bool force;
    bool from_ble;
    char to[OTA_VERSION_MAX];
};

static volatile pull_state_t s_state = PS_IDLE;

// Traspaso de una sola ranura, tarea BLE → tarea de bucle (mismo patrón que
// ota.cpp, líneas 45-49).
static pull_req_t    s_ctrl_req;
static volatile bool s_ctrl_pending = false;
static portMUX_TYPE  s_ctrl_mux = portMUX_INITIALIZER_UNLOCKED;

// Traspaso de una sola ranura, tarea de bucle → tarea worker. El bucle solo
// rellena esto mientras el worker está parado; que net_worker_submit() falle es
// la red de seguridad de la sincronización.
static pull_req_t       s_run_req;

// Estado de la planificación (DESIGN §10.1).
static bool     s_auto = true;
static bool     s_boot_checked = false;
static uint32_t s_boot_ms = 0;
static uint32_t s_last_chk = 0;   // época de la última comprobación completada
static uint32_t s_defer = 0;      // época antes de la cual no corre ninguna comprobación automática
static uint8_t  s_chk_fail = 0;   // comprobaciones fallidas consecutivas
static char     s_etag[ETAG_MAX + 1] = { 0 };
static uint32_t s_next_sched_ms = 0;   // regulación del sondeo de planificación NVS/hora

// ---- Ayudas pequeñas -------------------------------------------------------

static void pull_send(const char* json) {
    Serial.printf("OTA: pull TX %s\n", json);
    ble_notify_status(json);
}

static void pull_send_err(const char* err) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\"}", err ? err : "unknown");
    pull_send(buf);
}

static void pull_send_checking(void) {
    pull_send("{\"ok\":true,\"cmd\":\"update\",\"state\":\"checking\"}");
}

static void pull_send_up_to_date(const char* ver) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"update\",\"state\":\"up_to_date\",\"version\":\"%s\"}", ver);
    pull_send(buf);
}

static void pull_send_available(const char* ver, long size) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"update\",\"state\":\"available\",\"version\":\"%s\",\"size\":%ld}",
             ver, size);
    pull_send(buf);
}

// "Actualización <versión>" en la línea transitoria de la interfaz cuando hay
// una compilación más reciente disponible (DESIGN §12). Es puramente añadido: la
// respuesta BLE de arriba es la fuente de verdad.
static void pull_ui_available(const char* ver) {
    char line[OTA_VERSION_MAX + 16];
    snprintf(line, sizeof(line), "Actualización %s", ver);
    ui_ota_status(line, -1);
}

static void pull_send_downloading(int pct) {
    ui_ota_status("Actualizando", pct);   // línea transitoria; nunca bloquea al worker
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"update\",\"state\":\"downloading\",\"pct\":%d}", pct);
    pull_send(buf);
}

static void pull_send_verifying(void) {
    pull_send("{\"ok\":true,\"cmd\":\"update\",\"state\":\"verifying\"}");
}

static void pull_send_rebooting(const char* ver) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"update\",\"state\":\"rebooting\",\"version\":\"%s\"}", ver);
    pull_send(buf);
}

static bool pull_load_creds(char* ssid, size_t sn, char* pass, size_t pn) {
    ssid[0] = '\0';
    pass[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(PULL_NS, true)) return false;
    String s = prefs.getString(K_SSID, "");
    String p = prefs.getString(K_PASS, "");
    prefs.end();
    strlcpy(ssid, s.c_str(), sn);
    strlcpy(pass, p.c_str(), pn);
    return ssid[0] != '\0';
}

static void pull_nvs_u32(const char* key, uint32_t value) {
    Preferences prefs;
    if (!prefs.begin(PULL_NS, false)) return;
    prefs.putUInt(key, value);
    prefs.end();
}

static void pull_nvs_u8(const char* key, uint8_t value) {
    Preferences prefs;
    if (!prefs.begin(PULL_NS, false)) return;
    prefs.putUChar(key, value);
    prefs.end();
}

// Una comprobación completada y correcta.
static void pull_record_ok(void) {
    time_t now = time(nullptr);
    if ((long)now > MIN_VALID_EPOCH) s_last_chk = (uint32_t)now;
    s_defer = 0;
    s_chk_fail = 0;
    pull_nvs_u32(K_LAST_CHK, s_last_chk);
    pull_nvs_u32(K_DEFER, 0);
    pull_nvs_u8(K_CHK_FAIL, 0);
    s_boot_checked = true;
}

// Una comprobación completada con un manifiesto o imagen permanentemente malos
// — no reintentar.
static void pull_record_terminal(void) {
    ui_ota_status("No se actualizó", -1);   // línea transitoria corta, DESIGN §12
    time_t now = time(nullptr);
    if ((long)now > MIN_VALID_EPOCH) s_last_chk = (uint32_t)now;
    s_defer = 0;
    s_chk_fail = 0;
    pull_nvs_u32(K_LAST_CHK, s_last_chk);
    pull_nvs_u32(K_DEFER, 0);
    pull_nvs_u8(K_CHK_FAIL, 0);
    s_boot_checked = true;
}

// Un fallo transitorio: espera exponencial, se persiste `defer` para que un
// reinicio a mitad de la espera no machaque la VM (§7.3).
static void pull_record_transient(void) {
    time_t now = time(nullptr);
    uint32_t epoch = ((long)now > MIN_VALID_EPOCH) ? (uint32_t)now : 0;
    if (s_chk_fail < 255) s_chk_fail++;
    if (s_chk_fail >= 5) {
        // Rendirse hasta la siguiente cadencia.
        s_chk_fail = 0;
        if (epoch) s_last_chk = epoch;
        s_defer = 0;
    } else if (epoch) {
        s_defer = epoch + kCheckBackoffS[s_chk_fail - 1];
    }
    pull_nvs_u32(K_LAST_CHK, s_last_chk);
    pull_nvs_u32(K_DEFER, s_defer);
    pull_nvs_u8(K_CHK_FAIL, s_chk_fail);
    s_boot_checked = true;
}

static void pull_defer_battery(void) {
    time_t now = time(nullptr);
    if ((long)now > MIN_VALID_EPOCH) {
        s_defer = (uint32_t)now + BATTERY_DEFER_S;
        pull_nvs_u32(K_DEFER, s_defer);
    }
}

static void pull_fail_transient(const pull_req_t& req, const char* err) {
    pull_record_transient();
    if (req.from_ble) {
        pull_send_err(err);
    } else {
        Serial.printf("OTA: pull check failed (%s), deferred\n", err ? err : "unknown");
    }
}

// Comprobación de placa contra el manifiesto en tiempo de compilación, pero con
// la versión en ejecución interpretada de forma laxa: una compilación de
// desarrollo ("0.0.0-dev") no es semver estricto, así que se trata como 0.0.0 al
// comparar en lugar de envenenar todos los resultados.
static int pull_version_cmp(const char* newer, const char* running) {
    int parsed[3];
    if (!semver_parse(running, parsed)) return semver_cmp(newer, "0.0.0");
    return semver_cmp(newer, running);
}

// Puerta de batería / carga (DESIGN §10.2). `for_apply` elige el suelo más alto;
// `bypass` (fuerza del dueño u obligatorio por manifiesto) lo baja al suelo de
// comprobación.
static bool pull_battery_ok(bool for_apply, bool bypass) {
    if (!board_caps().has_battery) return true;
    int pct = power_hal_battery_pct();
    if (pct < 0) return true;  // desconocido — no bloquear por una lectura que falta
    if (power_hal_is_charging() || power_hal_is_vbus_in()) return true;
    int floor = (for_apply && !bypass) ? APPLY_FLOOR_PCT : CHECK_FLOOR_PCT;
    return pct >= floor;
}

static const char* pull_manifest_err_name(int e) {
    switch (e) {
    case OTA_MF_BAD_SCHEMA:  return "bad_schema";
    case OTA_MF_BAD_BOARD:   return "board_mismatch";
    case OTA_MF_BAD_VERSION: return "bad_version";
    case OTA_MF_BAD_URL:     return "bad_url";
    case OTA_MF_BAD_SHA:     return "bad_schema";
    case OTA_MF_BAD_SIZE:    return "bad_schema";
    case OTA_MF_BAD_SIG:     return "bad_signature";
    case OTA_MF_BAD_JSON:    return "bad_schema";
    default:                 return "bad_schema";
    }
}

// ---- Unión a WiFi / SNTP ----------------------------------------------------

static bool pull_wifi_join(const char* ssid, const char* pass) {
    for (int attempt = 0; attempt < 3; attempt++) {
        Serial.printf("OTA: pull join attempt %d heap=%u max=%u\n",
                      attempt + 1, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        WiFi.persistent(false);
        if (!WiFi.mode(WIFI_STA)) {
            Serial.println("OTA: WiFi.mode(WIFI_STA) failed (radio/heap)");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        WiFi.begin(ssid, pass);
        uint32_t deadline = millis() + WIFI_JOIN_MS;
        while ((int32_t)(millis() - deadline) < 0) {
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("OTA: pull WiFi up (%s)\n", WiFi.localIP().toString().c_str());
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        Serial.printf("OTA: pull join attempt %d failed (status=%d)\n",
                      attempt + 1, (int)WiFi.status());
        WiFi.disconnect(true);
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(kJoinBackoffMs[attempt]));
    }
    return false;
}

static bool pull_sntp(void) {
    // Usar IPs de servidores NTP, no nombres: en algunas redes locales el DNS
    // es lento o el primer configTime() no tiene nada que resolver, así que la
    // sincronización se pasa de tiempo. Tres IP anycast muy conocidas
    // (Google / Cloudflare) más una de reserva con nombre.
    static const char* const srv[] = {
        "216.239.35.0",     // time.google.com
        "162.159.200.1",    // time.cloudflare.com
        "pool.ntp.org",
    };
    for (int attempt = 0; attempt < 4; attempt++) {
        configTime(0, 0, srv[attempt % 3], srv[(attempt + 1) % 3], srv[(attempt + 2) % 3]);
        uint32_t deadline = millis() + SNTP_TIMEOUT_MS;
        while ((int32_t)(millis() - deadline) < 0) {
            if ((long)time(nullptr) > MIN_VALID_EPOCH) {
                Serial.printf("OTA: pull clock synced (%ld)\n", (long)time(nullptr));
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        Serial.printf("OTA: pull SNTP attempt %d timed out\n", attempt + 1);
    }
    return false;
}

// ---- Descarga del manifiesto ----------------------------------------------

enum fetch_res_t { FETCH_OK, FETCH_304, FETCH_TLS, FETCH_HTTP, FETCH_PARSE };

static fetch_res_t pull_fetch_manifest(OtaManifest* mf, int* http_code, int* parse_err) {
    *http_code = 0;
    *parse_err = OTA_MF_OK;

    char url[OTA_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/%s/manifest.json",
             OTA_PULL_MANIFEST_URL, board_caps().id);
    Serial.printf("OTA: pull GET %s\n", url);

    WiFiClientSecure client;
    client.setCACert(PINNED_SERVER_PEM);   // certificado autofirmado anclado, §8
    HTTPClient http;
    http.setTimeout(MANIFEST_TIMEOUT_MS);
    if (!http.begin(client, url)) {
        http.end();
        return FETCH_TLS;
    }
    if (s_etag[0]) http.addHeader("If-None-Match", s_etag);
    int code = http.GET();
    *http_code = code;

    if (code == 304) {           // ETag aún válido → nada que actualizar (§6.3)
        http.end();
        return FETCH_304;
    }
    if (code != 200) {
        Serial.printf("OTA: pull manifest HTTP %d\n", code);
        http.end();
        return (code < 0) ? FETCH_TLS : FETCH_HTTP;
    }

    String body = http.getString();
    String etag = http.header("ETag");
    http.end();

    if (body.length() == 0) return FETCH_HTTP;
    ota_manifest_err pe = ota_manifest_parse(body.c_str(), mf);
    if (pe != OTA_MF_OK) {
        *parse_err = (int)pe;
        Serial.printf("OTA: pull manifest parse err %d\n", (int)pe);
        return FETCH_PARSE;
    }
    if (etag.length() > 0) {     // guardarlo para If-None-Match
        strlcpy(s_etag, etag.c_str(), sizeof(s_etag));
        Preferences prefs;
        if (prefs.begin(PULL_NS, false)) {
            prefs.putString(K_ETAG, s_etag);
            prefs.end();
        }
    }
    return FETCH_OK;
}

// ---- Comparación ----------------------------------------------------------

enum compare_res_t { CR_APPLY, CR_CHECK_ONLY, CR_UP_TO_DATE, CR_BATTERY_LOW, CR_REJECT };

static compare_res_t pull_compare(const pull_req_t& req, const OtaManifest& mf,
                                  const char** err_out) {
    // Nunca flashear una placa ajena (DESIGN §6.2).
    if (strcmp(mf.board, board_caps().id) != 0) {
        *err_out = "board_mismatch";
        return CR_REJECT;
    }
    // La fuerza del dueño debe nombrar exactamente la versión del manifiesto
    // (§5.4).
    if (req.force && req.to[0] && strcmp(req.to, mf.version) != 0) {
        *err_out = "target_mismatch";
        return CR_REJECT;
    }
    // Cordura de released_at: rechazar un manifiesta más de 24 h en el futuro
    // (§6.2).
    time_t now = time(nullptr);
    if (mf.released_at[0] && (long)now > MIN_VALID_EPOCH &&
        !ota_released_at_sane(mf.released_at, (long)now)) {
        *err_out = "bad_schema";
        return CR_REJECT;
    }

    int cmp = pull_version_cmp(mf.version, ota_version());
    if (cmp <= 0 && !req.force) {
        return CR_UP_TO_DATE;   // igual o más antigua es un "no actualizar" terminal (§5.4)
    }
    // min_from nunca se salta por fuerza del dueño.
    if (!req.force && mf.min_from[0] &&
        pull_version_cmp(mf.min_from, ota_version()) > 0) {
        *err_out = "too_old";
        return CR_REJECT;
    }

    // Puerta temprana de política de firmas (SIGNING.md §4 regla 2), decidida
    // AQUÍ, en COMPARE, y no después de la descarga: una clave anclada sin `sig`
    // en el manifiesto nunca podrá activarse, así que se rechaza antes de la
    // transferencia de ~3 MB (CR_REJECT -> pull_send_err("bad_signature") +
    // pull_record_terminal(), la misma clase terminal que `hash_mismatch`). Un
    // `sig` presente pero inválido sigue detectándolo la comprobación
    // criptográfica de pull_verify(). Sin clave anclada (SIGNING_PUBKEY_PEM
    // vacío) esto se omite: solo hash, sin cambios.
    if (SIGNING_PUBKEY_PEM[0] != '\0' && mf.sig[0] == '\0') {
        *err_out = "bad_signature";
        return CR_REJECT;
    }

    // Una comprobación solo informa: nunca necesita el suelo de aplicar, más
    // exigente.
    if (!req.apply) return CR_CHECK_ONLY;

    bool bypass = req.force || mf.mandatory;   // solo puertas de batería / prioridad
    if (!pull_battery_ok(true, bypass)) return CR_BATTERY_LOW;

    return CR_APPLY;
}

// ---- Descarga + verificación ----------------------------------------------

struct dl_ctx {
    const esp_partition_t* part;
    esp_ota_handle_t       handle;
    mbedtls_sha256_context sha;
    bool                   sha_open;
    bool                   ota_open;
    bool                   force_bypass;
    long                   total;
    char                   err[24];
};

static void dl_set_err(dl_ctx* c, const char* e) {
    strlcpy(c->err, e ? e : "timeout", sizeof(c->err));
}

static void pull_download_cleanup(dl_ctx* c) {
    if (c->sha_open) {
        mbedtls_sha256_free(&c->sha);
        c->sha_open = false;
    }
    if (c->ota_open) {
        esp_ota_abort(c->handle);
        c->ota_open = false;
    }
}

// Un intento: descargar el binario con GET y volcarlo en la ranura inactiva
// mientras se actualiza un SHA-256 en streaming. Deja el manejador OpenOTA y el
// resumen abiertos para pull_verify() si todo va bien.
static bool pull_download_once(dl_ctx* c, const OtaManifest& mf) {
    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!part || part == running) {   // invariante anti-brick §7.2
        Serial.println("OTA: pull no valid inactive slot");
        dl_set_err(c, "bad_image");
        return false;
    }
    c->part = part;

    char url[OTA_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/%s/%s",
             OTA_PULL_MANIFEST_URL, board_caps().id, mf.url);
    Serial.printf("OTA: pull GET %s (%ld bytes)\n", url, mf.size);

    WiFiClientSecure client;
    client.setCACert(PINNED_SERVER_PEM);   // certificado autofirmado anclado, §8
    HTTPClient http;
    http.setTimeout(DOWNLOAD_IDLE_MS);
    if (!http.begin(client, url)) {
        http.end();
        dl_set_err(c, "tls_fail");
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        Serial.printf("OTA: pull bin HTTP %d\n", code);
        http.end();
        dl_set_err(c, (code == 404) ? "http_404" : (code < 0 ? "tls_fail" : "timeout"));
        return false;
    }
    int clen = http.getSize();
    if (clen >= 0 && (long)clen != mf.size) {
        Serial.printf("OTA: pull size mismatch clen=%d manifest=%ld\n", clen, mf.size);
        http.end();
        dl_set_err(c, "bad_image");
        return false;
    }

    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &c->handle) != ESP_OK) {
        Serial.println("OTA: pull esp_ota_begin failed");
        http.end();
        dl_set_err(c, "bad_image");
        return false;
    }
    c->ota_open = true;

    mbedtls_sha256_init(&c->sha);
    mbedtls_sha256_starts(&c->sha, 0);
    c->sha_open = true;

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[4096];
    long total = 0;
    int last_pct = -1;
    uint32_t started = millis();
    uint32_t last_data = started;
    bool failed = false;

    while (total < mf.size) {
        if ((uint32_t)(millis() - started) > DOWNLOAD_TOTAL_MS) { dl_set_err(c, "timeout"); failed = true; break; }
        if ((uint32_t)(millis() - last_data) > DOWNLOAD_IDLE_MS) { dl_set_err(c, "timeout"); failed = true; break; }
        if (!pull_battery_ok(true, c->force_bypass)) { dl_set_err(c, "battery_low"); failed = true; break; }

        size_t want = sizeof(buf);
        if ((long)want > mf.size - total) want = (size_t)(mf.size - total);
        size_t n = stream->readBytes(buf, want);
        if (n == 0) {
            if (!http.connected()) { dl_set_err(c, "timeout"); failed = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        last_data = millis();
        if (esp_ota_write(c->handle, buf, n) != ESP_OK) { dl_set_err(c, "bad_image"); failed = true; break; }
        mbedtls_sha256_update(&c->sha, buf, n);
        total += (long)n;

        int pct = (int)((total * 100) / (mf.size > 0 ? mf.size : 1));
        if (pct != last_pct && (pct == 100 || pct - last_pct >= 5)) {
            last_pct = pct;
            pull_send_downloading(pct);
        }
    }
    http.end();

    if (failed) return false;
    if (total != mf.size) {   // cuerpo corto sin un error de flujo explícito
        dl_set_err(c, "timeout");
        return false;
    }
    c->total = total;
    return true;
}

static bool pull_download(dl_ctx* c, const OtaManifest& mf, bool bypass) {
    memset(c, 0, sizeof(*c));
    c->force_bypass = bypass;

    for (int attempt = 0; attempt < 2; attempt++) {   // §7.1: 2 descargas completas
        if (pull_download_once(c, mf)) return true;
        bool battery_low = (strcmp(c->err, "battery_low") == 0);
        char saved[sizeof(c->err)];
        strlcpy(saved, c->err, sizeof(saved));
        pull_download_cleanup(c);
        memset(c, 0, sizeof(*c));
        c->force_bypass = bypass;
        strlcpy(c->err, saved, sizeof(c->err));

        if (battery_low || attempt >= 1) return false;
        Serial.printf("OTA: pull download attempt %d failed (%s), retrying\n",
                      attempt + 1, c->err);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return false;
}

// Comprobación de firma separada (design/ota-pull/SIGNING.md sección 7). Corre
// sobre EL MISMO resumen SHA-256 de 32 bytes que acaba de validar la comprobación
// de hash: la firma autentica el resumen, y el resumen autentica los bytes.
//
// Política (SIGNING.md sección 4, regla 2):
//   * sin clave anclada (SIGNING_PUBKEY_PEM vacío): compilación previa a P4 solo
//     con hash; la comprobación se omite y se registra.
//   * clave anclada, manifiesto sin firmar: se rechaza como `bad_signature`. Este
//     caso normalmente se decide antes, en COMPARE (pull_compare), así que no se
//     inicia ninguna descarga; la comprobación de abajo queda como red de
//     seguridad defensiva para cualquier futuro llamador que llegue a VERIFY sin
//     pasar por COMPARE.
//   * clave anclada, manifiesto firmado: `ota_sig_verify_p256()` debe aceptarlo.
// Ante cualquier rechazo, quien llama aborta el manejador OTA y nunca activa.
static bool pull_verify_signature(const OtaManifest& mf, const uint8_t* digest,
                                  const char** err) {
    const bool have_key = (SIGNING_PUBKEY_PEM[0] != '\0');

    // Puerta de política pura compartida con la prueba de host: sin clave ->
    // siempre correcta. El caso "clave anclada pero sin firma" normalmente ya se
    // rechaza en COMPARE (antes de la descarga); llegar hasta aquí significaría
    // que un llamador se saltó esa puerta, así que falla de forma dura en lugar
    // de caer hasta la criptografía.
    if (!ota_manifest_sig_ok(&mf, have_key)) {
        Serial.println("OTA: pull manifest unsigned but a signing key is pinned, rejecting");
        *err = "bad_signature";
        return false;
    }
    if (!have_key) {
        Serial.println("OTA: pull signing not configured, hash-only verification");
        return true;
    }

    // El analizador ya aplicó la sintaxis de sig_alg y de base64 (OTA_MF_BAD_SIG);
    // aquí se decodifica a DER. Un búfer de 128 bytes cubre la firma DER P-256
    // más grande.
    uint8_t sig_der[OTA_SIG_B64_MAX];
    int sig_len = ota_base64_decode(mf.sig, sig_der, sizeof(sig_der));
    if (sig_len <= 0) {
        Serial.println("OTA: pull manifest sig base64 decode failed");
        *err = "bad_signature";
        return false;
    }

    if (!ota_sig_verify_p256(digest, 32, sig_der, (size_t)sig_len, SIGNING_PUBKEY_PEM)) {
        Serial.printf("OTA: pull signature verify failed (key_id=%s)\n", mf.key_id);
        *err = "bad_signature";
        return false;
    }
    Serial.printf("OTA: pull signature OK (alg=%s key_id=%s)\n", mf.sig_alg, mf.key_id);
    return true;
}

// Terminar el resumen, compararlo con el manifiesto, verificar la firma
// separada (cuando está anclada) y luego validar la imagen. El hash y la firma
// se comprueban antes de esp_ota_end() para que un fallo pueda hacer
// esp_ota_abort() sobre el manejador (DESIGN §7.1 / §11, SIGNING.md §7) — en
// ningún caso se activa nada.
static bool pull_verify(dl_ctx* c, const OtaManifest& mf, const char** err) {
    uint8_t digest[32];
    mbedtls_sha256_finish(&c->sha, digest);
    mbedtls_sha256_free(&c->sha);
    c->sha_open = false;

    char hex[OTA_SHA256_HEX_LEN + 1];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[OTA_SHA256_HEX_LEN] = '\0';

    if (!ota_sha256_hex_eq(hex, mf.sha256)) {
        Serial.println("OTA: pull SHA-256 mismatch, aborting");
        esp_ota_abort(c->handle);
        c->ota_open = false;
        *err = "hash_mismatch";
        return false;
    }

    // Comprobación de firma: después del SHA-256, antes de esp_ota_end() y de
    // la activación. Un fallo es terminal (`bad_signature`), de la misma clase
    // que `hash_mismatch`.
    if (!pull_verify_signature(mf, digest, err)) {
        esp_ota_abort(c->handle);
        c->ota_open = false;
        return false;
    }

    esp_err_t e = esp_ota_end(c->handle);
    c->ota_open = false;
    if (e != ESP_OK) {
        Serial.printf("OTA: pull esp_ota_end failed (%d)\n", (int)e);
        *err = "bad_image";
        return false;
    }
    return true;
}

// ---- Activación ------------------------------------------------------------

static bool pull_activate(const OtaManifest& mf, const esp_partition_t* part) {
    // Recordar la versión pendiente antes de tocar otadata; ota_confirm() la
    // limpia una vez que arranca la imagen nueva (DESIGN §7.2).
    Preferences prefs;
    if (prefs.begin(PULL_NS, false)) {
        prefs.putString(K_PEND_VER, mf.version);
        prefs.end();
    }
    esp_err_t e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        Serial.printf("OTA: pull set_boot_partition failed (%d)\n", (int)e);
        return false;
    }
    return true;
}

// ---- Worker -----------------------------------------------------------------

// Corre en el worker de OTA por consulta (nunca en la tarea de bucle ni en la de
// NimBLE). Las llamadas a ui_ota_status() de abajo son la única interacción con
// la interfaz: cada una solo guarda una línea corta para la tarea de bucle
// (DESIGN §12), así que no bloquean y un panel apagado o dormido nunca afecta al
// flujo de control.
static void pull_cycle_inner(const pull_req_t& req) {
    char ssid[64] = { 0 }, pass[64] = { 0 };
    if (!pull_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        pull_send_err("no_wifi");
        return;
    }

    s_state = PS_JOIN;
    ui_ota_status("Comprobando", -1);
    if (!pull_wifi_join(ssid, pass)) { pull_fail_transient(req, "timeout"); return; }

    s_state = PS_SNTP;
    ui_ota_status("Comprobando", -1);
    if (!pull_sntp()) { pull_fail_transient(req, "timeout"); return; }

    OtaManifest mf;
    memset(&mf, 0, sizeof(mf));
    int http_code = 0, parse_err = OTA_MF_OK;
    s_state = PS_MANIFEST;
    ui_ota_status("Comprobando", -1);
    fetch_res_t fr = pull_fetch_manifest(&mf, &http_code, &parse_err);
    if (fr == FETCH_304) {
        pull_send_up_to_date(ota_version());
        pull_record_ok();
        return;
    }
    if (fr != FETCH_OK) {
        const char* e = "timeout";
        bool terminal = false;
        if (fr == FETCH_PARSE) {
            e = pull_manifest_err_name(parse_err);
            terminal = true;
        } else if (fr == FETCH_HTTP) {
            if (http_code == 404) { e = "http_404"; terminal = true; }
        } else {  // FETCH_TLS
            e = "tls_fail";
        }
        if (terminal) { pull_send_err(e); pull_record_terminal(); }
        else          { pull_fail_transient(req, e); }
        return;
    }

    s_state = PS_COMPARE;
    const char* cerr = "no_update";
    compare_res_t cr = pull_compare(req, mf, &cerr);
    if (cr == CR_UP_TO_DATE) {
        pull_send_up_to_date(ota_version());   // informar de lo que corre de verdad
        pull_record_ok();
        return;
    }
    if (cr == CR_REJECT) {
        pull_send_err(cerr);
        pull_record_terminal();
        return;
    }
    if (cr == CR_BATTERY_LOW) {
        if (req.from_ble && req.apply) pull_send_err("battery_low");
        else {
            pull_send_available(mf.version, mf.size);
            pull_ui_available(mf.version);
        }
        pull_defer_battery();
        return;
    }
    pull_send_available(mf.version, mf.size);
    pull_ui_available(mf.version);
    if (cr == CR_CHECK_ONLY) {
        pull_record_ok();
        return;
    }

    dl_ctx dl;
    s_state = PS_DOWNLOAD;
    ui_ota_status("Actualizando", -1);
    if (!pull_download(&dl, mf, req.force || mf.mandatory)) {
        const char* e = dl.err[0] ? dl.err : "timeout";
        bool battery_low = (strcmp(e, "battery_low") == 0);
        if (battery_low) {
            if (req.from_ble) pull_send_err("battery_low");
            else {
                pull_send_available(mf.version, mf.size);
                pull_ui_available(mf.version);
            }
            pull_defer_battery();
        } else {
            pull_fail_transient(req, e);
        }
        return;
    }

    s_state = PS_VERIFY;
    ui_ota_status("Verificando", -1);
    pull_send_verifying();
    const char* verr = "bad_image";
    if (!pull_verify(&dl, mf, &verr)) {
        // hash_mismatch / bad_signature / bad_image no se reintentan (§7.3).
        pull_send_err(verr);
        pull_record_terminal();
        return;
    }

    s_state = PS_ACTIVATE;
    if (!pull_activate(mf, dl.part)) {
        pull_send_err("activate_fail");
        pull_record_terminal();
        return;
    }

    s_state = PS_REBOOT;
    ui_ota_status("Reiniciando", -1);
    pull_send_rebooting(mf.version);
    pull_record_ok();
    ota_wifi_release();                 // apagar la radio antes del reinicio (§10.3)
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    Serial.printf("OTA: pull rebooting into %s\n", mf.version);
    esp_restart();
}

static void pull_run_cycle(void) {
    pull_req_t req = s_run_req;   // el worker está parado; el bucle escribió esto antes de despertarlo
    if (!ota_wifi_acquire(OTA_WIFI_PULL)) {
        pull_send_err("busy");
        s_state = PS_IDLE;
        return;
    }
    pull_cycle_inner(req);
    ota_wifi_release();
    s_state = PS_IDLE;
}

// Cuerpo del trabajo para el worker de red compartido (net_worker.cpp). Corre con
// la única pila compartida de 12 KB de RAM interna; s_state vuelve a PS_IDLE
// cuando regresa.
static void ota_pull_job(void* arg) {
    (void)arg;
    pull_run_cycle();
}

// ---- API pública ------------------------------------------------------------

void ota_pull_init(void) {
    s_boot_ms = millis();

    Preferences prefs;
    if (prefs.begin(PULL_NS, true)) {
        s_auto = prefs.getUChar(K_AUTO, 1) != 0;
        s_last_chk = prefs.getUInt(K_LAST_CHK, 0);
        s_defer = prefs.getUInt(K_DEFER, 0);
        s_chk_fail = prefs.getUChar(K_CHK_FAIL, 0);
        String etag = prefs.getString(K_ETAG, "");
        strlcpy(s_etag, etag.c_str(), sizeof(s_etag));
        prefs.end();
    }

    // Sin tarea por módulo: el worker bloqueante es la tarea compartida
    // net_worker, creada una sola vez en setup(). Esto mantiene una ÚNICA pila de
    // 12 KB de RAM interna residente para los tres caminos de consulta.

    Serial.printf("OTA: pull init auto=%d last_chk=%lu defer=%lu chk_fail=%u etag=%s\n",
                  (int)s_auto, (unsigned long)s_last_chk, (unsigned long)s_defer,
                  (unsigned)s_chk_fail, s_etag[0] ? s_etag : "-");
}

void ota_pull_tick(void) {
    if (s_state != PS_IDLE || ota_is_active()) return;   // ocupado, o la híbrida es dueña de la radio
    // No consumir una petición BLE de un solo tiro mientras el worker compartido
    // siga ocupado con otra consulta; la petición se queda en cola en
    // s_ctrl_pending.
    if (net_worker_busy()) return;

    pull_req_t req = {};
    bool have = false;
    bool is_ble = false;

    // Una petición BLE en cola tiene prioridad sobre la planificación y se
    // responde de inmediato; a la planificación le basta con resolución de
    // segundos.
    portENTER_CRITICAL(&s_ctrl_mux);
    if (s_ctrl_pending) {
        req = s_ctrl_req;
        s_ctrl_pending = false;
        have = true;
        is_ble = true;
    }
    portEXIT_CRITICAL(&s_ctrl_mux);

    if (!have) {
        if (!s_auto) return;
        if ((int32_t)(millis() - s_next_sched_ms) < 0) return;
        s_next_sched_ms = millis() + 5000;

        if ((uint32_t)(millis() - s_boot_ms) < HEALTHY_MS) return;   // primero se confirma el arranque

        time_t now = time(nullptr);
        bool clock_ok = (long)now > MIN_VALID_EPOCH;
        if (s_defer && clock_ok && (uint32_t)now < s_defer) return;  // ventana de espera

        char ssid[64], pass[64];
        if (!pull_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) return;  // sin credenciales

        bool boot_due = !s_boot_checked;
        bool timer_due = clock_ok &&
                         (s_last_chk == 0 ||
                          ((uint32_t)now - s_last_chk) >= CHECK_INTERVAL_S);
        if (!boot_due && !timer_due) return;

        req.apply = true;
        req.force = false;
        req.from_ble = false;
        req.to[0] = '\0';
        have = true;
    }
    if (!have) return;

    // Las puertas corren aquí, en la tarea de bucle, nunca en la de NimBLE. El
    // suelo es el barato de comprobación (20 %); el suelo de aplicar, más
    // exigente (50 %), se aplica después en COMPARE, para que una comprobación
    // pueda seguir informando de `available` (§10.2).
    char ssid[64], pass[64];
    if (!pull_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        if (is_ble) pull_send_err("no_wifi");
        return;
    }
    bool applies = req.from_ble ? req.apply : true;   // las comprobaciones por BLE se saltan el suelo
    if (applies && !pull_battery_ok(false, req.force)) {
        if (is_ble) {
            pull_send_err("battery_low");
        } else {
            pull_defer_battery();
            Serial.println("OTA: pull auto check deferred (battery low)");
        }
        s_boot_checked = true;
        return;
    }

    bool had_boot_check = s_boot_checked;
    s_run_req = req;
    s_boot_checked = true;
    s_state = PS_JOIN;   // exponer "active" a la interfaz antes de que corra el worker
    if (!net_worker_submit(ota_pull_job, nullptr)) {
        // Ranura ocupada (defensivo: arriba ya se comprobó net_worker_busy()). Se
        // revierte para que el siguiente tick lo reintente en lugar de perder la
        // comprobación.
        s_boot_checked = had_boot_check;
        s_state = PS_IDLE;
        s_next_sched_ms = millis() + 1000;
    }
}

void ota_pull_handle_ctrl(const char* json) {
    if (!json) return;

    // Tarea de host de NimBLE: solo analizar y encolar. Todas las puertas que
    // tocan NVS o el PMU corren en la tarea de bucle, en ota_pull_tick()
    // (DESIGN §9, IMPL §2.3).
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        pull_send_err("bad_json");
        return;
    }

    if (ota_pull_is_active() || ota_is_active()) {
        pull_send_err("busy");
        return;
    }

    const char* mode = doc["mode"] | "";
    bool apply = !(mode[0] && strcmp(mode, "check") == 0);

    pull_req_t req = {};
    req.apply = apply;
    req.force = doc["force"] | false;
    req.from_ble = true;
    strlcpy(req.to, doc["to"] | "", sizeof(req.to));

    portENTER_CRITICAL(&s_ctrl_mux);
    s_ctrl_req = req;
    s_ctrl_pending = true;
    portEXIT_CRITICAL(&s_ctrl_mux);

    pull_send_checking();
}

bool ota_pull_is_active(void) {
    return s_state != PS_IDLE;
}

const char* ota_pull_state_name(void) {
    switch (s_state) {
    case PS_JOIN:     return "join";
    case PS_SNTP:     return "sntp";
    case PS_MANIFEST: return "manifest";
    case PS_COMPARE:  return "manifest";
    case PS_DOWNLOAD: return "download";
    case PS_VERIFY:   return "verify";
    case PS_ACTIVATE: return "activate";
    case PS_REBOOT:   return "reboot";
    case PS_IDLE:
    default:          return "idle";
    }
}
