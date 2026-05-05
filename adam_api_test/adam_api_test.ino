/*
 * adam_api_test.ino
 *
 * Haalt de ingestelde watertemperatuur (intended_boiler_temperature) op
 * van de Plugwise Adam via de lokale REST API.
 *
 * Endpoint: GET http://<adam-ip>/core/domain_objects
 * Auth    : HTTP Basic, gebruikersnaam altijd "smile",
 *           wachtwoord staat op de sticker van de Adam.
 *
 * De XML-response wordt streaming gelezen — geen grote buffer nodig.
 * Gezocht wordt naar het patroon:
 *   <type>intended_boiler_temperature</type>
 *   ... (enkele tags)
 *   <measurement log_date="...">35.0</measurement>
 */

#include <WiFiS3.h>

// ── Configuratie ──────────────────────────────────────────────────────────────
const char* SSID      = "jouw-netwerk";
const char* PASS      = "jouw-wachtwoord";
const char* ADAM_IP   = "192.168.1.x";   // IP-adres van de Adam
const char* ADAM_PASS = "jouw-smile-ww"; // wachtwoord op sticker van Adam

const unsigned long POLL_MS = 30000;     // ophaalinterval (ms)

// ── Base64 (voor HTTP Basic Auth) ─────────────────────────────────────────────
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64(const String& s) {
    String out;
    int len = s.length();
    const uint8_t* in = (const uint8_t*)s.c_str();
    for (int i = 0; i < len; i += 3) {
        uint8_t a = in[i], b = (i+1 < len) ? in[i+1] : 0, c = (i+2 < len) ? in[i+2] : 0;
        out += B64[a >> 2];
        out += B64[((a & 3) << 4) | (b >> 4)];
        out += (i+1 < len) ? B64[((b & 0xF) << 2) | (c >> 6)] : '=';
        out += (i+2 < len) ? B64[c & 0x3F]                     : '=';
    }
    return out;
}

// ── Streaming zoekfunctie ─────────────────────────────────────────────────────
// Leest client karakter voor karakter en geeft true als 'needle' gevonden is.
bool stream_find(WiFiClient& client, const char* needle, unsigned long timeout_ms) {
    int n = strlen(needle), idx = 0;
    unsigned long t = millis();
    while (millis() - t < timeout_ms) {
        if (!client.available()) { delay(1); continue; }
        char c = client.read();
        idx = (c == needle[idx]) ? idx + 1 : (c == needle[0] ? 1 : 0);
        if (idx == n) return true;
    }
    return false;
}

// ── Adam ophalen ──────────────────────────────────────────────────────────────
float fetch_water_setpoint() {
    WiFiClient client;
    if (!client.connect(ADAM_IP, 80)) {
        Serial.println("Adam: verbinding mislukt");
        return NAN;
    }

    String auth = base64("smile:" + String(ADAM_PASS));

    // HTTP/1.0 voorkomt chunked transfer encoding
    client.println("GET /core/domain_objects HTTP/1.0");
    client.println("Host: " + String(ADAM_IP));
    client.println("Authorization: Basic " + auth);
    client.println("Connection: close");
    client.println();

    // Wacht op response
    unsigned long t0 = millis();
    while (!client.available() && millis() - t0 < 5000) delay(1);

    // Sla HTTP-headers over (zoek lege regel \r\n\r\n)
    if (!stream_find(client, "\r\n\r\n", 5000)) {
        Serial.println("Adam: geen headers ontvangen");
        client.stop();
        return NAN;
    }

    // Zoek XML-tag 'intended_boiler_temperature' in de body
    if (!stream_find(client, "intended_boiler_temperature", 15000)) {
        Serial.println("Adam: intended_boiler_temperature niet gevonden");
        client.stop();
        return NAN;
    }

    // Zoek het openende <measurement-attribuut (heeft log_date="..." dus geen exacte tag)
    if (!stream_find(client, "<measurement", 5000)) {
        Serial.println("Adam: <measurement> niet gevonden");
        client.stop();
        return NAN;
    }

    // Sla attributen over, wacht op sluitende '>' van de openingstag
    if (!stream_find(client, ">", 2000)) {
        client.stop();
        return NAN;
    }

    // Lees de waarde tot het sluitende '<'
    String value;
    t0 = millis();
    while (millis() - t0 < 2000) {
        if (!client.available()) { delay(1); continue; }
        char c = client.read();
        if (c == '<') break;
        value += c;
    }

    client.stop();
    value.trim();
    if (value.length() == 0) return NAN;
    return value.toFloat();
}

// ── Setup & loop ──────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.print("WiFi verbinden");
    WiFi.begin(SSID, PASS);
    while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
    Serial.println(" OK  IP=" + WiFi.localIP().toString());
}

unsigned long vorige_poll = -POLL_MS;  // meteen eerste poll bij start

void loop() {
    if (millis() - vorige_poll >= POLL_MS) {
        vorige_poll = millis();

        float t = fetch_water_setpoint();
        if (!isnan(t)) {
            Serial.print("Water setpoint (Adam): ");
            Serial.print(t, 1);
            Serial.println(" °C");
        } else {
            Serial.println("Water setpoint: niet beschikbaar");
        }
    }
}
