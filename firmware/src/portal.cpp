// SoftAP + captive portal WiFi provisioning. Contract:
// design/backend-wifi/ROADMAP.md (D6 and section 7).
//
// Hardware-only module: the native sim links the no-op stubs in
// boards/sim/ota_sim.cpp instead (platformio.ini excludes this file), exactly
// like ota.cpp / ota_pull.cpp.
//
// Flow:
//   * main.cpp auto-starts the portal ~5 s after boot when no credentials are
//     stored; the owner can also request it over BLE/serial with
//     {"cmd":"portal"} (or stop it with {"cmd":"portal","mode":"off"}).
//   * portal_start()/portal_stop() are enqueued from the NimBLE host task and
//     executed here in portal_tick() on the Arduino loop task.
//   * The device advertises "Clawdmeter-XXXX" (XXXX = last two MAC bytes), runs
//     a wildcard DNSServer that points every name at the portal IP and a
//     WebServer on :80 serving a self-contained form (plus the backend pairing
//     code while the device is unpaired). POSTing the form writes the
//     credentials through ota_set_wifi() (the one NVS write path) and stops the
//     AP after a short linger. A 5 min deadline stops an unattended portal.
//   * The portal never runs while a pull OTA or the hybrid OTA owns the radio.
//
// Memory: internal RAM is tight. The static HTML lives in flash (.rodata) and
// is streamed with sendContent_P() in Content-Length'd chunks — only the small
// pairing-code block (~120 B) and the two short form fields ever become heap
// Strings, and they are copied straight into ota_set_wifi()'s fixed buffers.
#include "portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "ota.h"
#include "ota_pull.h"
#include "usage_pair.h"

// Format the 8-char backend pairing code as "ABCD-2345" for the page. The
// phase-2 alphabet (PHASE2-CONTRACT.md §2) has no HTML-special characters, so
// the result is safe to interpolate into the markup unchanged.
static void format_pair_code(const char* code, char* buf, size_t len) {
    if (!code || !code[0]) { buf[0] = '\0'; return; }
    if (strlen(code) == 8) snprintf(buf, len, "%.4s-%.4s", code, code + 4);
    else                   snprintf(buf, len, "%s", code);
}

// NVS location frozen by the OTA contract (design/ota-hybrid/DESIGN.md §2/§4,
// the same keys ota.cpp reads/writes). Read-only here — writes go through
// ota_set_wifi().
#define PORTAL_NVS_NS  "otah"
#define PORTAL_KEY_SSID "ssid"

#define PORTAL_TIMEOUT_MS     (5u * 60u * 1000u)  // unattended portal lifetime
#define PORTAL_SAVE_LINGER_MS 2000u               // stay up briefly after a save

enum portal_req_t : uint8_t { PORTAL_REQ_NONE = 0, PORTAL_REQ_START, PORTAL_REQ_STOP };

// ---- Pages (flash-resident) ------------------------------------------------

static const char PORTAL_HTML_HEAD[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Clawdmeter WiFi</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;background:#0d1117;color:#e6edf3}"
    "main{max-width:22rem;margin:8vh auto;padding:1.5rem;background:#161b22;border-radius:12px}"
    "h1{font-size:1.25rem;margin:0 0 .25rem}p{color:#8b949e;font-size:.85rem}"
    "label{display:block;margin:.9rem 0 .3rem;font-size:.8rem;color:#8b949e}"
    "input{width:100%;box-sizing:border-box;padding:.6rem;border:1px solid #30363d;"
    "border-radius:8px;background:#0d1117;color:#e6edf3;font-size:1rem}"
    "button{width:100%;margin-top:1.1rem;padding:.7rem;border:0;border-radius:8px;"
    "background:#238636;color:#fff;font-size:1rem}"
    ".s{font-size:.75rem;color:#6e7681;margin-top:1rem;line-height:1.4}"
    ".pc{margin:.2rem 0 1rem;padding:.75rem;background:#0d1117;border:1px dashed #d97757;"
    "border-radius:8px;text-align:center}"
    ".pc .k{font-size:.7rem;text-transform:uppercase;letter-spacing:.08em;color:#8b949e}"
    ".pc .c{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:1.6rem;"
    "font-weight:700;letter-spacing:.12em;color:#faf9f5;margin:.25rem 0}"
    ".pc .e{font-size:.75rem;color:#8b949e;margin:.35rem 0 0}"
    "</style></head><body><main>"
    "<h1>Clawdmeter setup</h1><p>Connect this device to your WiFi network.</p>";

// The form + footer are static. The pairing-code block that belongs between the
// two halves is runtime state, so handle_root() streams the page in three
// Content-Length'd chunks rather than building a full-page String.
static const char PORTAL_HTML_TAIL[] PROGMEM =
    "<form method=\"POST\" action=\"/save\">"
    "<label for=\"ssid\">Network name (SSID)</label>"
    "<input id=\"ssid\" name=\"ssid\" maxlength=\"63\" required autocapitalize=\"off\" "
    "autocorrect=\"off\" autocomplete=\"off\">"
    "<label for=\"pass\">Password</label>"
    "<input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"63\">"
    "<button type=\"submit\">Save &amp; connect</button></form>"
    "<p class=\"s\">Saved to this device only. If this page does not open "
    "automatically, browse to <b>http://192.168.4.1</b></p>"
    "</main></body></html>";

static const char SAVED_HTML[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Saved</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;background:#0d1117;color:#e6edf3;"
    "text-align:center}div{max-width:22rem;margin:10vh auto;padding:1.5rem}"
    "h1{font-size:1.25rem}p{color:#8b949e}</style></head><body><div>"
    "<h1>Saved &#10003;</h1><p>Connecting to your network&hellip; "
    "you can close this tab.</p></div></body></html>";

static const char ERROR_HTML[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Missing SSID</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;background:#0d1117;color:#e6edf3;"
    "text-align:center}div{max-width:22rem;margin:10vh auto;padding:1.5rem}"
    "a{color:#58a6ff}</style></head><body><div>"
    "<h1>SSID required</h1><p>Please enter your network name.</p>"
    "<p><a href=\"/\">Try again</a></p></div></body></html>";

// ---- State -----------------------------------------------------------------

static WebServer s_http(80);
static DNSServer s_dns;

static char     s_ssid[32] = {0};  // "Clawdmeter-XXXX"
static bool     s_active = false;
static uint32_t s_deadline = 0;    // absolute ms: unattended auto-stop
static uint32_t s_stop_at = 0;     // absolute ms: linger after a successful save
static uint8_t  s_req = PORTAL_REQ_NONE;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// ---- HTTP handlers (run on the loop task via portal_tick) -------------------

static void handle_root(void) {
    // The pairing code is runtime state, so stream the page as three
    // Content-Length'd chunks: the flash-resident head, a small dynamic code
    // block, and the flash-resident form/footer. Chunking the body (rather than
    // building one full-page String) keeps the bulk in flash and stays
    // compatible with HTTP/1.0 clients, unlike a chunked Transfer-Encoding.
    const char* code = usage_pair_code();
    char block[384];
    size_t block_len = 0;
    if (code && code[0]) {
        char codebuf[16];
        format_pair_code(code, codebuf, sizeof(codebuf));
        int n = snprintf(block, sizeof(block),
            "<div class=\"pc\"><div class=\"k\">Pairing code</div>"
            "<div class=\"c\">%s</div>"
            "<p class=\"e\">Enter this code with the owner tool to pair this device.</p>"
            "</div>", codebuf);
        if (n > 0) block_len = ((size_t)n < sizeof(block)) ? (size_t)n : sizeof(block) - 1;
    }

    size_t head_len = strlen_P(PORTAL_HTML_HEAD);
    size_t tail_len = strlen_P(PORTAL_HTML_TAIL);
    s_http.setContentLength(head_len + block_len + tail_len);
    s_http.send(200, "text/html", "");
    s_http.sendContent_P(PORTAL_HTML_HEAD);
    if (block_len) s_http.sendContent(block, block_len);
    s_http.sendContent_P(PORTAL_HTML_TAIL);
}

static void handle_save(void) {
    String ssid = s_http.arg("ssid");
    String pass = s_http.arg("pass");
    ssid.trim();
    if (ssid.length() == 0) {
        s_http.send_P(400, "text/html", ERROR_HTML);
        return;
    }
    // Single NVS write path (no duplicated Preferences logic here).
    ota_set_wifi(ssid.c_str(), pass.c_str());
    Serial.printf("PORTAL: saved creds ssid=%s\n", ssid.c_str());
    s_http.send_P(200, "text/html", SAVED_HTML);
    s_stop_at = millis() + PORTAL_SAVE_LINGER_MS;
}

// Every unknown path — including the OS captive-portal probes
// (/generate_204, /hotspot-detect.html, /connecttest.txt, …) — is bounced to
// the form so the popup/portal experience is consistent. The wildcard DNS
// resolves the probe's hostname to us so the request lands here.
static void handle_probe(void) {
    s_http.sendHeader("Location", "http://192.168.4.1/", true);
    s_http.send(302, "text/plain", "");
}

// ---- Radio bring-up / teardown ---------------------------------------------

const char* portal_ssid(void) {
    if (!s_ssid[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_ssid, sizeof(s_ssid), "Clawdmeter-%02X%02X", mac[4], mac[5]);
    }
    return s_ssid;
}

bool portal_has_creds(void) {
    Preferences prefs;
    prefs.begin(PORTAL_NVS_NS, true);
    String ssid = prefs.getString(PORTAL_KEY_SSID, "");
    prefs.end();
    return ssid.length() > 0;
}

static void portal_start_now(void) {
    const char* ssid = portal_ssid();
    IPAddress ip(192, 168, 4, 1);

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(ssid)) {
        Serial.println("PORTAL: softAP failed");
        WiFi.mode(WIFI_OFF);
        return;
    }

    s_dns.start(53, "*", ip);
    s_http.begin();
    s_active = true;
    s_stop_at = 0;
    s_deadline = millis() + PORTAL_TIMEOUT_MS;
    Serial.printf("PORTAL: SoftAP \"%s\" up at %s (%us)\n",
                  ssid, ip.toString().c_str(), (unsigned)(PORTAL_TIMEOUT_MS / 1000));
}

static void portal_stop_now(void) {
    s_http.stop();
    s_dns.stop();
    // Only drop the radio if no OTA owner took it over in the meantime.
    if (!ota_is_active() && !ota_pull_is_active()) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    s_active = false;
    s_deadline = 0;
    s_stop_at = 0;
    Serial.println("PORTAL: SoftAP down");
}

// ---- Public API ------------------------------------------------------------

void portal_init(void) {
    // Register the handlers once; WebServer persists them across begin()/stop(),
    // so a later portal session only has to call begin() again.
    s_http.on("/", HTTP_GET, handle_root);
    s_http.on("/save", HTTP_POST, handle_save);
    s_http.onNotFound(handle_probe);
    Serial.printf("PORTAL: init ssid=%s creds=%s\n",
                  portal_ssid(), portal_has_creds() ? "stored" : "none");
}

void portal_start(void) {
    portENTER_CRITICAL(&s_mux);
    s_req = PORTAL_REQ_START;
    portEXIT_CRITICAL(&s_mux);
}

void portal_stop(void) {
    portENTER_CRITICAL(&s_mux);
    s_req = PORTAL_REQ_STOP;
    portEXIT_CRITICAL(&s_mux);
}

bool portal_is_active(void) { return s_active; }

void portal_tick(void) {
    portENTER_CRITICAL(&s_mux);
    uint8_t req = s_req;
    s_req = PORTAL_REQ_NONE;
    portEXIT_CRITICAL(&s_mux);

    if (req == PORTAL_REQ_STOP) {
        if (s_active) portal_stop_now();
    } else if (req == PORTAL_REQ_START && !s_active) {
        if (ota_is_active() || ota_pull_is_active()) {
            Serial.println("PORTAL: start refused (OTA active)");
        } else {
            portal_start_now();
        }
    }

    if (!s_active) return;

    // An OTA that grabbed the radio wins: close the portal but leave the radio
    // alone — the OTA owner tears it down itself.
    if (ota_is_active() || ota_pull_is_active()) {
        Serial.println("PORTAL: OTA active — closing portal");
        s_http.stop();
        s_dns.stop();
        s_active = false;
        s_deadline = 0;
        s_stop_at = 0;
        return;
    }

    if (s_stop_at && (int32_t)(millis() - s_stop_at) >= 0) {
        Serial.println("PORTAL: configured — stopping AP");
        portal_stop_now();
        return;
    }
    if ((int32_t)(millis() - s_deadline) >= 0) {
        Serial.println("PORTAL: timeout — stopping AP");
        portal_stop_now();
        return;
    }

    s_dns.processNextRequest();
    s_http.handleClient();
}
