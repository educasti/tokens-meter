#include "pf_privacy.h"
#include <Preferences.h>
#include <Arduino.h>

// El mismo espacio de nombres NVS y el mismo estilo de clave que el `brt_idx` de
// brightness.cpp: las dos son preferencias locales del usuario que tienen que
// sobrevivir a un reinicio sin reflasheo.
#define PF_NVS_NS   "clawdmeter"
#define PF_NVS_KEY  "pf_priv"

// La línea transitoria de "Modo privado" / "Modo normal" dura exactamente lo
// mismo que los puntos de página, y por el mismo motivo: el usuario tiene el
// hábito de "PWR = brillo", y esto es lo que le explica en el acto por qué la
// pantalla acaba de cambiar.
#define PF_NOTICE_MS 1500u

// Por defecto desactivado: el primer arranque tiene que parecerse a cualquier
// otro.
static bool     pf_private = false;
static uint32_t notice_ms  = 0;

void pf_privacy_init(void) {
    Preferences prefs;
    prefs.begin(PF_NVS_NS, true);
    pf_private = prefs.getUChar(PF_NVS_KEY, 0) != 0;
    prefs.end();
    Serial.printf("Portfolio privacy: %s\n", pf_private ? "privado" : "normal");
}

void pf_privacy_toggle(void) {
    pf_private = !pf_private;

    Preferences prefs;
    prefs.begin(PF_NVS_NS, false);
    prefs.putUChar(PF_NVS_KEY, pf_private ? 1 : 0);
    prefs.end();

    notice_ms = millis();
    Serial.printf("Portfolio privacy toggled: %s\n", pf_private ? "privado" : "normal");
}

bool pf_privacy_get(void) {
    return pf_private;
}

bool pf_privacy_notice(void) {
    return (uint32_t)(millis() - notice_ms) < PF_NOTICE_MS;
}
