/*
 * adam_api_test.ino  —  Plugwise Adam lokale REST API
 *
 * Leest intended_boiler_temperature van de Plugwise Adam.
 * Dit is het aanvoer setpoint dat Adam via OpenTherm naar de ketel stuurt
 * (0 = geen warmtevraag, >0 = gewenste aanvoertemperatuur in grC).
 *
 * Integratie in chofu_wp_ff:
 *   t_water_gewenst = adam_sp_aanvoer;   // in WATER / FF_WATER modus
 *   wp_uit = (adam_sp_aanvoer == 0);     // geen warmtevraag van Adam
 *
 * Kamertemperatuur en setpoint komen via MQTT (Anna thermostaat),
 * niet via Adam API — de XML is 200+ KB en zones staan pas achteraan.
 *
 * Maak adam_api_test/config.h aan:
 *   #define SSID      "netwerk"
 *   #define PASS      "wachtwoord"
 *   #define ADAM_IP   "192.168.1.x"
 *   #define ADAM_PASS "smile-ww"   // 8 tekens van sticker
 */

#if defined(ARDUINO_UNOR4_WIFI)
  #include <WiFiS3.h>
#else
  #include <WiFi.h>
#endif

#if __has_include("config.h")
  #include "config.h"
#else
  #define SSID      "jouw-netwerk"
  #define PASS      "jouw-wachtwoord"
  #define ADAM_IP   "192.168.1.x"
  #define ADAM_PASS "jouw-smile-ww"
#endif

const unsigned long POLL_MS    = 30000;
const unsigned long TIMEOUT_MS = 15000;

// ── Base64 ────────────────────────────────────────────────────────────────────
static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64(const String& s) {
  String out;
  const uint8_t* in = (const uint8_t*)s.c_str();
  int len = s.length();
  for (int i = 0; i < len; i += 3) {
    uint8_t a=in[i], b=(i+1<len)?in[i+1]:0, c=(i+2<len)?in[i+2]:0;
    out += B64[a>>2]; out += B64[((a&3)<<4)|(b>>4)];
    out += (i+1<len) ? B64[((b&0xF)<<2)|(c>>6)] : '=';
    out += (i+2<len) ? B64[c&0x3F]               : '=';
  }
  return out;
}

// ── Streaming helpers ─────────────────────────────────────────────────────────

// Wacht tot data beschikbaar is; false als verbinding verbroken of timeout
bool stream_wait(WiFiClient& cl, unsigned long ms) {
  unsigned long t = millis();
  while (!cl.available()) {
    if (!cl.connected())        return false;
    if (millis()-t > ms)        return false;
    delay(1);
  }
  return true;
}

bool stream_find(WiFiClient& cl, const char* needle, unsigned long ms) {
  int n=strlen(needle), idx=0;
  unsigned long t=millis();
  while (millis()-t < ms) {
    if (!cl.available()) {
      if (!cl.connected()) return false;   // verbinding verbroken
      delay(1); continue;
    }
    char c = cl.read();
    if (c == needle[idx]) { if (++idx == n) return true; }
    else idx = (c == needle[0]) ? 1 : 0;
  }
  return false;
}

float stream_read_measurement(WiFiClient& cl, unsigned long ms) {
  if (!stream_find(cl, "<measurement", ms)) return NAN;
  if (!stream_find(cl, ">",            500)) return NAN;
  char buf[16] = {};
  int i = 0;
  unsigned long t = millis();
  while (millis()-t < 1000 && i < 15) {
    if (!cl.available()) { delay(1); continue; }
    char c = cl.read();
    if (c == '<') break;
    buf[i++] = c;
  }
  String s = buf; s.trim();
  return s.length() ? s.toFloat() : NAN;
}

// ── Ophaalfunctie ─────────────────────────────────────────────────────────────
// intended_boiler_temperature staat op ~32KB in de 215KB XML.
// Zones staan pas op ~43KB en later — die lezen we hier niet.

float fetch_adam_setpoint() {
  WiFiClient cl;
  if (!cl.connect(ADAM_IP, 80)) {
    Serial.println("[Adam] verbinding mislukt"); return NAN;
  }

  String auth = base64("smile:" + String(ADAM_PASS));
  unsigned long t0 = millis();

  cl.println("GET /core/domain_objects HTTP/1.1");
  cl.print("Host: "); cl.println(ADAM_IP);
  cl.println("Authorization: Basic " + auth);
  cl.println("Connection: close");
  cl.println();

  while (!cl.available() && millis()-t0 < 5000) delay(1);
  if (!stream_find(cl, "\r\n\r\n", 5000)) {
    Serial.println("[Adam] geen headers"); cl.stop(); return NAN;
  }

  // Zoek direct naar intended_boiler_temperature (staat op ~32KB, ~14% van XML)
  if (!stream_find(cl, "<type>intended_boiler_temperature</type>", TIMEOUT_MS)) {
    Serial.println("[Adam] intended_boiler_temperature niet gevonden");
    cl.stop(); return NAN;
  }

  float sp = stream_read_measurement(cl, 3000);

  cl.stop();
  Serial.print("[Adam] duur: "); Serial.print(millis()-t0); Serial.println(" ms");
  return sp;
}

// ── Setup & loop ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Adam aanvoer setpoint test");
  Serial.print("WiFi verbinden");
  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.println(" OK  IP=" + WiFi.localIP().toString());
}

unsigned long vorige_poll = (unsigned long)-POLL_MS;

void loop() {
  if (millis() - vorige_poll >= POLL_MS) {
    vorige_poll = millis();

    float sp = fetch_adam_setpoint();

    if (!isnan(sp)) {
      if (sp == 0.0f) {
        Serial.println("Adam: geen warmtevraag (SP=0)");
      } else {
        Serial.print("Adam aanvoer SP: "); Serial.print(sp, 1); Serial.println(" grC");
      }
      // Integratie chofu_wp_ff:
      // t_water_gewenst = sp;
      // if (sp == 0) ctrl.zet_uit();
    } else {
      Serial.println("[Adam] geen data");
    }
  }
}
