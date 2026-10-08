// OTA híbrido — transferencia del binario por WiFi sobre un canal de control BLE.
//
// Contrato congelado: design/ota-hybrid/DESIGN.md. Módulo solo de hardware: el
// simulador nativo enlaza boards/sim/ota_sim.cpp en su lugar (ver el
// build_src_filter del entorno de simulación). El reparto de tareas importa —
// ota_handle_ctrl() corre en la tarea de host de NimBLE y solo analiza/encola;
// todas las llamadas a WiFi y a ArduinoOTA ocurren en ota_tick(), en la tarea de
// bucle de Arduino, cuya pila puede absorber una unión y un arranque de mDNS.
#include "ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "ble.h"
#include "hal/board_caps.h"
#include "ota_pull.h"
#include "ota_wifi.h"
#include "portal.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"   // se fija por entorno en platformio.ini (-DFW_VERSION="...")
#endif
#ifndef FW_GIT_SHA
#define FW_GIT_SHA ""      // lo inyecta scripts/version.py (sello de compilación P0)
#endif
#ifndef FW_BUILD_DATE
#define FW_BUILD_DATE ""   // lo inyecta scripts/version.py (sello de compilación P0)
#endif

// La ubicación y las claves de NVS las congela el contrato (§2 / §4).
#define OTAH_NS        "otah"
#define KEY_SSID       "ssid"
#define KEY_PASS       "pass"
#define KEY_BOOT_TRIES "boot_tries"

#define OTA_PORT       3232
#define WIFI_JOIN_MS   20000u   // unión acotada; para entonces el ayudante ya se ha rendido
#define HEALTHY_MS     60000u   // tiempo activo que confirma un arranque sin carga del dueño
#define MAX_BOOT_TRIES 3        // más arranques sin confirmar que esto → volver atrás

enum ota_mode_t { OTA_MODE_IDLE, OTA_MODE_CONNECTING, OTA_MODE_READY };
enum ota_req_t   { REQ_NONE, REQ_START, REQ_STOP, REQ_REBOOT };

static ota_mode_t s_state = OTA_MODE_IDLE;

// Traspaso de peticiones de una sola ranura desde la tarea BLE a la tarea de
// bucle. Gana la más reciente: el ayudante solo tiene un comando de control en
// vuelo a la vez.
static ota_req_t     s_req = REQ_NONE;
static char          s_req_pass[64];
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

static char     s_ssid[64] = {0};
static char     s_pass[64] = {0};
static char     s_ota_pass[64] = {0};   // contraseña de la sesión OTA en curso
static bool     s_ota_pass_set = false;
static uint32_t s_connect_deadline = 0;
static uint32_t s_boot_ms = 0;
static bool     s_boot_confirmed = false;

const char* ota_version(void) { return FW_VERSION; }

// ---- NVS: credenciales de WiFi ---------------------------------------------

static void load_creds(void) {
    Preferences prefs;
    prefs.begin(OTAH_NS, true);
    String ssid = prefs.getString(KEY_SSID, "");
    String pass = prefs.getString(KEY_PASS, "");
    prefs.end();
    strlcpy(s_ssid, ssid.c_str(), sizeof(s_ssid));
    strlcpy(s_pass, pass.c_str(), sizeof(s_pass));
    Serial.printf("OTA: stored creds %s\n", s_ssid[0] ? s_ssid : "(none)");
}

void ota_set_wifi(const char* ssid, const char* pass) {
    strlcpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    Preferences prefs;
    prefs.begin(OTAH_NS, false);
    prefs.putString(KEY_SSID, s_ssid);
    prefs.putString(KEY_PASS, s_pass);
    prefs.end();
    Serial.printf("OTA: wifi creds stored (ssid=%s)\n", s_ssid);
}

void ota_wifi_clear(void) {
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    Preferences prefs;
    prefs.begin(OTAH_NS, false);
    prefs.remove(KEY_SSID);
    prefs.remove(KEY_PASS);
    prefs.end();
    Serial.println("OTA: wifi creds cleared");
}

// ---- NVS: contador de verificación de arranque + reversión a nivel de aplicación (§4) ---

static void boot_counter_reset(void) {
    Preferences prefs;
    prefs.begin(OTAH_NS, false);
    prefs.putUChar(KEY_BOOT_TRIES, 0);
    prefs.end();
}

// El bootloader de Arduino precompilado no corre con
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE verificado, así que la reversión es a
// nivel de aplicación: apuntar otadata a la otra ranura de aplicación y
// reiniciar. La secuencia del propio Espressif — cambiar de partición y luego
// poner el contador a cero — evita que la ranura buena revierta de inmediato a
// la mala (ping-pong).
static void rollback_and_restart(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* other   = esp_ota_get_next_update_partition(running);
    if (!other || other == running) {
        Serial.println("OTA: rollback: no alternate slot; keeping current image");
        boot_counter_reset();
        return;
    }
    Serial.printf("OTA: rollback %s -> %s\n",
                  running ? running->label : "?", other->label);
    if (esp_ota_set_boot_partition(other) != ESP_OK) {
        Serial.println("OTA: rollback: set_boot_partition failed; keeping current image");
        boot_counter_reset();
        return;
    }
    boot_counter_reset();
    delay(100);
    esp_restart();
}

void ota_confirm(void) {
    if (s_boot_confirmed) return;
    s_boot_confirmed = true;
    boot_counter_reset();
    Serial.println("OTA: boot confirmed (boot_tries=0)");
}

void ota_init(void) {
    s_boot_ms = millis();
    load_creds();

    Preferences prefs;
    prefs.begin(OTAH_NS, false);
    uint8_t tries = prefs.getUChar(KEY_BOOT_TRIES, 0) + 1;
    prefs.putUChar(KEY_BOOT_TRIES, tries);
    prefs.end();
    Serial.printf("OTA: boot verify armed, boot_tries=%u\n", tries);

    if (tries > MAX_BOOT_TRIES) rollback_and_restart();
    // Si la reversión no puede seguir, caemos hacia abajo y seguimos corriendo; la
    // confirmación de 60 s / por carga de abajo dejará el contador en su sitio.
}

// ---- Traspaso de peticiones (tarea BLE → tarea de bucle) --------------------

static void enqueue(ota_req_t r, const char* pass) {
    portENTER_CRITICAL(&s_mux);
    s_req = r;
    if (r == REQ_START) strlcpy(s_req_pass, pass ? pass : "", sizeof(s_req_pass));
    portEXIT_CRITICAL(&s_mux);
}

static ota_req_t dequeue(char* pass_out, size_t n) {
    portENTER_CRITICAL(&s_mux);
    ota_req_t r = s_req;
    s_req = REQ_NONE;
    if (r == REQ_START) {
        strlcpy(pass_out, s_req_pass, n);
        s_req_pass[0] = '\0';
    }
    portEXIT_CRITICAL(&s_mux);
    return r;
}

void ota_start(const char* pass) { enqueue(REQ_START, pass); }
void ota_stop(void)              { enqueue(REQ_STOP, nullptr); }
bool ota_is_active(void)         { return s_state != OTA_MODE_IDLE; }

// ---- Notificaciones de estado por TX (§2) -----------------------------------

static void send_status(const char* json) {
    Serial.printf("OTA: TX %s\n", json);
    ble_notify_status(json);
}

static void send_ready(void) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"cmd\":\"ota\",\"state\":\"ready\",\"ip\":\"%s\",\"port\":%d}",
             WiFi.localIP().toString().c_str(), OTA_PORT);
    send_status(buf);
}

static void send_off(void) {
    send_status("{\"ok\":true,\"cmd\":\"ota\",\"state\":\"off\"}");
}

// ---- WiFi / ArduinoOTA ------------------------------------------------------

// clawdmeter-<últimos6delamac> (§3), en hexadecimal minúsculo. La cadena de MAC
// que viene de ble.cpp está en mayúsculas: "AA:BB:CC:DD:EE:FF".
static void make_hostname(char* out, size_t n) {
    const char* mac = ble_get_mac_address();
    char hex[13];
    int j = 0;
    for (int i = 0; mac && mac[i] && j < 12; i++) {
        char c = mac[i];
        if (c == ':') continue;
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        hex[j++] = c;
    }
    hex[j] = '\0';
    const char* tail = (j >= 6) ? hex + j - 6 : hex;
    snprintf(out, n, "clawdmeter-%s", tail);
}

static bool wifi_begin(void) {
    // La radio tiene un único dueño (DESIGN §7.4): negarse a arrancar si el motor
    // de consulta ya la tiene, reflejando la respuesta "busy" ante un arranque
    // doble.
    if (!ota_wifi_acquire(OTA_WIFI_HYBRID)) {
        Serial.println("OTA: wifi busy (held by another owner)");
        send_status("{\"ok\":false,\"err\":\"busy\"}");
        return false;
    }
    Serial.printf("OTA: joining WiFi ssid=%s\n", s_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(s_ssid, s_pass);
    s_connect_deadline = millis() + WIFI_JOIN_MS;
    s_state = OTA_MODE_CONNECTING;
    return true;
}

static void ota_server_begin(const char* pass) {
    char host[32];
    make_hostname(host, sizeof(host));
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setHostname(host);
    // setPassword() calcula el hash SHA256 de su argumento; una cadena vacía daría
    // un hash real de 64 caracteres (es decir, seguiría pidiendo autenticación),
    // así que una sesión sin autenticar nunca la llama. (ArduinoOTA solo ofrece
    // fijar/sobrescribir, así que un cambio de sesión de "password" a "none"
    // dentro de un mismo arranque conserva el hash antiguo — el ayudante es
    // coherente entre sus reinicios escalonados, así que en la práctica no es un
    // problema.)
    if (pass && pass[0]) ArduinoOTA.setPassword(pass);
    ArduinoOTA.setRebootOnSuccess(true);
    ArduinoOTA.onStart([]() { Serial.println("OTA: transfer started"); });
    ArduinoOTA.onEnd([]() { Serial.println("OTA: transfer ended"); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static unsigned int last = 0;
        if (total && (done - last) * 100 / total >= 10) {
            last = done;
            Serial.printf("OTA: transfer %u%%\n", done * 100 / total);
        }
    });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA: error %u\n", (unsigned)e); });
    ArduinoOTA.begin();
    Serial.printf("OTA: server ready at %s.local:%d ip=%s\n",
                  host, OTA_PORT, WiFi.localIP().toString().c_str());
}

static void ota_stop_now(void) {
    if (s_state == OTA_MODE_READY) ArduinoOTA.end();
    ota_wifi_release();   // disconnect(true) + WiFi.mode(WIFI_OFF)
    s_state = OTA_MODE_IDLE;
    s_ota_pass_set = false;
    s_ota_pass[0] = '\0';
    Serial.println("OTA: stopped, WiFi down");
}

// ---- Analizador de comandos CTRL (§2) ---------------------------------------

void ota_handle_ctrl(const char* json) {
    if (!json) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("OTA: ctrl bad json: %s\n", err.c_str());
        send_status("{\"ok\":false,\"err\":\"bad_json\"}");
        return;
    }

    const char* cmd = doc["cmd"] | "";
    Serial.printf("OTA: ctrl cmd=%s\n", cmd);

    if (strcmp(cmd, "info") == 0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "{\"ok\":true,\"board\":\"%s\",\"fw\":\"%s\",\"sha\":\"%s\",\"build\":\"%s\",\"id\":\"%s\"}",
                 board_caps().id, ota_version(), FW_GIT_SHA, FW_BUILD_DATE,
                 ble_get_mac_address());
        send_status(buf);
        return;
    }

    if (strcmp(cmd, "wifi") == 0) {
        ota_set_wifi(doc["ssid"] | "", doc["pass"] | "");
        send_status("{\"ok\":true,\"cmd\":\"wifi\"}");
        return;
    }

    if (strcmp(cmd, "wifi_clear") == 0) {
        ota_wifi_clear();
        send_status("{\"ok\":true,\"cmd\":\"wifi_clear\"}");
        return;
    }

    if (strcmp(cmd, "portal") == 0) {
        // Solo encolar: portal_start()/portal_stop() ponen una bandera y el SoftAP
        // se levanta/cierra desde portal_tick() en la tarea de bucle. Desde esta
        // tarea (host de NimBLE) nunca se toca WiFi.
        const char* mode = doc["mode"] | "";
        if (strcmp(mode, "off") == 0) {
            portal_stop();
            send_status("{\"ok\":true,\"cmd\":\"portal\",\"state\":\"off\"}");
        } else if (ota_pull_is_active() || ota_is_active()) {
            send_status("{\"ok\":false,\"err\":\"busy\"}");
        } else {
            portal_start();
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"cmd\":\"portal\",\"state\":\"on\",\"ssid\":\"%s\"}",
                     portal_ssid());
            send_status(buf);
        }
        return;
    }

    if (strcmp(cmd, "ota") == 0) {
        const char* mode = doc["mode"] | "";
        if (strcmp(mode, "on") == 0) {
            if (!s_ssid[0]) {
                send_status("{\"ok\":false,\"err\":\"no_wifi\"}");
            } else if (portal_is_active()) {
                // El AP de aprovisionamiento es dueño de la radio; una sesión STA la
                // pisaría.
                send_status("{\"ok\":false,\"err\":\"portal\"}");
            } else {
                ota_start(doc["pass"] | "");   // la respuesta ready/error llega desde ota_tick()
            }
        } else if (strcmp(mode, "off") == 0) {
            ota_stop();                        // la respuesta "off" llega desde ota_tick()
        } else {
            send_status("{\"ok\":false,\"err\":\"bad_mode\"}");
        }
        return;
    }

    if (strcmp(cmd, "update") == 0) {
        // Comandos del motor de consulta (DESIGN §9): solo analizar/encolar; la
        // comprobación del dueño BLE ya se aplicó antes de ejecutar
        // ota_handle_ctrl().
        ota_pull_handle_ctrl(json);
        return;
    }

    if (strcmp(cmd, "reboot") == 0) {
        // El enlace se corta al reiniciar, así que a propósito no hay respuesta por
        // TX (§2).
        enqueue(REQ_REBOOT, nullptr);
        return;
    }

    send_status("{\"ok\":false,\"err\":\"unknown_cmd\"}");
}

// ---- Máquina de estados ----------------------------------------------------

void ota_tick(void) {
    // §4: 60 s de actividad sana confirman el arranque incluso sin carga.
    if (!s_boot_confirmed && (uint32_t)(millis() - s_boot_ms) >= HEALTHY_MS) {
        ota_confirm();
    }

    char pass[64] = {0};
    switch (dequeue(pass, sizeof(pass))) {
    case REQ_REBOOT:
        Serial.println("OTA: reboot requested");
        delay(50);
        esp_restart();
        break;

    case REQ_START:
        if (s_state == OTA_MODE_READY) {
            send_ready();   // ya está arriba — responde otra vez con la dirección actual
        } else if (s_state == OTA_MODE_CONNECTING) {
            send_status("{\"ok\":false,\"err\":\"busy\"}");
        } else {
            s_ota_pass_set = (pass[0] != '\0');
            strlcpy(s_ota_pass, pass, sizeof(s_ota_pass));
            wifi_begin();
        }
        break;

    case REQ_STOP:
        ota_stop_now();
        send_off();
        break;

    case REQ_NONE:
    default:
        break;
    }

    if (s_state == OTA_MODE_CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            ota_server_begin(s_ota_pass_set ? s_ota_pass : nullptr);
            s_state = OTA_MODE_READY;
            send_ready();
        } else if ((int32_t)(millis() - s_connect_deadline) >= 0) {
            Serial.printf("OTA: wifi join failed (status=%d)\n", (int)WiFi.status());
            ota_stop_now();
            send_status("{\"ok\":false,\"err\":\"wifi_timeout\"}");
        }
    }

    if (s_state == OTA_MODE_READY) ArduinoOTA.handle();
}
