#include "adam.h"
#include "protocol.h"   // jgc_ontvangend()
#include "mqtt.h"       // mqtt_log()
#include <math.h>
#include <stdlib.h>     // atof
#include <string.h>     // strcmp / strncpy / strlen

// ═══════════════════════════════════════════════════════════════
//  Configuratie (gezet door adam_init vanuit de .ino)
// ═══════════════════════════════════════════════════════════════
static bool        s_init       = false;
static const char* s_ip         = nullptr;
static const char* s_pass       = nullptr;
static const char* s_zones[ADAM_MAX_ZONES];
static uint8_t     s_zone_count = 0;

// ── Status ──────────────────────────────────────────────────────
static const char* s_status        = "uit";
static char        s_leider[24]    = "-";
static uint32_t    s_laatste_ok_ms = 0;
static uint8_t     s_fout_teller   = 0;

// ── Leer-gate stabiliteit ───────────────────────────────────────
#define ADAM_LEAD_STABIEL_MS  600000UL   // 10 min ongewijzigde leider vóór leren
#define ADAM_CALL_DREMPEL     0.1f        // °C deficit waarboven een zone 'vraagt'
static int8_t   s_prev_leader      = -1;  // cfg-index vorige leider
static uint32_t s_leider_sinds_ms  = 0;

// ═══════════════════════════════════════════════════════════════
//  Datastructuren
// ═══════════════════════════════════════════════════════════════
struct AdamZone {
  char   naam[24];
  int8_t cfg_idx;   // index in s_zones (tie-break), -1 = geen
  float  temp;
  float  sp;
  int8_t vraagt;    // -1 onbekend, 0 nee, 1 ja (uit control_state)
};

struct AdamData {
  float    water_sp;                 // intended_boiler_temperature (NAN = niet gevonden)
  float    outside;                  // outdoor_temperature (NAN = niet gevonden)
  AdamZone zones[ADAM_MAX_ZONES];
  uint8_t  n;
};

// ═══════════════════════════════════════════════════════════════
//  Base64 (Basic auth)
// ═══════════════════════════════════════════════════════════════
static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64(const String& s){
  String out;
  const uint8_t* in = (const uint8_t*)s.c_str();
  int len = s.length();
  for(int i = 0; i < len; i += 3){
    uint8_t a=in[i], b=(i+1<len)?in[i+1]:0, c=(i+2<len)?in[i+2]:0;
    out += B64[a>>2]; out += B64[((a&3)<<4)|(b>>4)];
    out += (i+1<len) ? B64[((b&0xF)<<2)|(c>>6)] : '=';
    out += (i+2<len) ? B64[c&0x3F]               : '=';
  }
  return out;
}

// ═══════════════════════════════════════════════════════════════
//  Lage-niveau stream helpers (met hard tijdsbudget)
// ═══════════════════════════════════════════════════════════════
// Eén byte lezen vóór deadline; -1 bij timeout of verbroken verbinding.
static int rd(WiFiClient& cl, uint32_t deadline){
  while(!cl.available()){
    if(!cl.connected())                       return -1;
    if((int32_t)(millis() - deadline) >= 0)   return -1;
    delay(1);
  }
  return cl.read();
}

// Zoek needle in de stream (voor het overslaan van HTTP-headers).
static bool stream_find(WiFiClient& cl, const char* needle, uint32_t deadline){
  int n = strlen(needle), idx = 0;
  for(;;){
    int ci = rd(cl, deadline);
    if(ci < 0) return false;
    char c = (char)ci;
    if(c == needle[idx]){ if(++idx == n) return true; }
    else idx = (c == needle[0]) ? 1 : 0;
  }
}

// eerste token (tagnaam) van een tag-string vergelijken
static bool tag_is(const char* tag, const char* naam){
  while(*naam){ if(*tag != *naam) return false; tag++; naam++; }
  // einde tagnaam = spatie, '/', of '\0'
  return (*tag == '\0' || *tag == ' ' || *tag == '/');
}

// ═══════════════════════════════════════════════════════════════
//  Pull-parser: één GET, capture water-SP + alle geconfigureerde zones
// ═══════════════════════════════════════════════════════════════
// XML is ~215 KB en ongesorteerd; we streamen token-voor-token (geen DOM) en
// stoppen zodra alles binnen is of het tijdsbudget op is.
//
// LET OP: gebaseerd op de structuur uit adam_discover.py (point_log type=
// temperature/thermostat, thermostat_functionality/setpoint). control_state als
// vraagsignaal is een HOOK — verifieer de exacte veldnaam met een echte XML-dump
// (adam_discover.py --raw) voordat je dit op hardware aanzet.

static bool adam_fetch(AdamData& out){
  out.water_sp = NAN;
  out.outside  = NAN;
  out.n = 0;

  WiFiClient cl;
  if(!cl.connect(s_ip, 80)) return false;

  uint32_t deadline = millis() + ADAM_TIMEOUT_MS;
  String auth = base64("smile:" + String(s_pass));

  cl.println("GET /core/domain_objects HTTP/1.1");
  cl.print("Host: "); cl.println(s_ip);
  cl.println("Authorization: Basic " + auth);
  cl.println("Connection: close");
  cl.println();

  if(!stream_find(cl, "\r\n\r\n", deadline)){ cl.stop(); return false; }

  // Parser-state
  char    txt[48];   uint8_t txi = 0;   // tekst tussen tags
  char    tag[48];                       // huidige tag
  char    last_type[40] = "";            // laatst gesloten <type>
  bool    in_loc   = false;              // binnen een echte <location>
  bool    want_name= false;              // volgende </name> levert de zonenaam
  AdamZone cur;                          // huidige (mogelijk te negeren) zone
  bool    boiler_pending = false;        // intended_boiler_temperature verwacht meting

  auto reset_cur = [&](){
    cur.naam[0] = '\0'; cur.cfg_idx = -1;
    cur.temp = NAN; cur.sp = NAN; cur.vraagt = -1;
  };
  reset_cur();

  for(;;){
    // 1) tekst tot '<'
    txi = 0;
    int ci;
    for(;;){
      ci = rd(cl, deadline);
      if(ci < 0){ cl.stop(); return (!isnan(out.water_sp) || out.n > 0); }
      if((char)ci == '<') break;
      if(txi < sizeof(txt) - 1) txt[txi++] = (char)ci;
    }
    txt[txi] = '\0';

    // 2) tag tot '>'
    uint8_t ti = 0;
    for(;;){
      ci = rd(cl, deadline);
      if(ci < 0){ cl.stop(); return (out.n > 0 || !isnan(out.water_sp)); }
      if((char)ci == '>') break;
      if(ti < sizeof(tag) - 1) tag[ti++] = (char)ci;
    }
    tag[ti] = '\0';
    bool self_close = (ti > 0 && tag[ti-1] == '/');

    // 3) verwerk
    if(tag_is(tag, "location") && !self_close){
      in_loc = true; want_name = false; reset_cur();
    }
    else if(tag_is(tag, "/location")){
      if(in_loc && cur.cfg_idx >= 0 && out.n < ADAM_MAX_ZONES){
        out.zones[out.n++] = cur;
      }
      in_loc = false;
    }
    else if(tag_is(tag, "name") && !self_close){
      if(in_loc) want_name = true;
    }
    else if(tag_is(tag, "/name")){
      if(in_loc && want_name){
        want_name = false;
        // match tegen geconfigureerde zones
        for(uint8_t k = 0; k < s_zone_count; k++){
          if(strcmp(txt, s_zones[k]) == 0){
            strncpy(cur.naam, txt, sizeof(cur.naam) - 1);
            cur.naam[sizeof(cur.naam)-1] = '\0';
            cur.cfg_idx = (int8_t)k;
            break;
          }
        }
      }
    }
    else if(tag_is(tag, "/type")){
      strncpy(last_type, txt, sizeof(last_type) - 1);
      last_type[sizeof(last_type)-1] = '\0';
      if(strcmp(last_type, "intended_boiler_temperature") == 0) boiler_pending = true;
    }
    else if(tag_is(tag, "/measurement")){
      float v = atof(txt);
      if(boiler_pending){
        out.water_sp = v; boiler_pending = false;
      } else if(strcmp(last_type, "outdoor_temperature") == 0){
        out.outside = v;                       // buitentemperatuur (gateway/Smile)
      } else if(in_loc && cur.cfg_idx >= 0){
        if(strcmp(last_type, "temperature") == 0) cur.temp = v;
        else if(strcmp(last_type, "thermostat") == 0 && isnan(cur.sp)) cur.sp = v;
      }
    }
    else if(tag_is(tag, "/setpoint")){
      // thermostat_functionality/setpoint = authoritatief kamer-setpoint
      if(in_loc && cur.cfg_idx >= 0) cur.sp = atof(txt);
    }
    else if(tag_is(tag, "/control_state")){
      // HOOK: vraagsignaal per zone (verifieer veldnaam met echte XML)
      if(in_loc && cur.cfg_idx >= 0) cur.vraagt = (strcmp(txt, "heating") == 0) ? 1 : 0;
    }

    // 4) klaar zodra alles binnen is (water-SP, buitentemp én alle zones)
    if(!isnan(out.water_sp) && !isnan(out.outside) && out.n >= s_zone_count){ break; }
  }

  cl.stop();
  return true;
}

// ═══════════════════════════════════════════════════════════════
//  Leidende zone kiezen + leer-gate bijwerken
// ═══════════════════════════════════════════════════════════════
static int kies_leider(const AdamData& d, uint8_t& callers_out){
  int   leader = -1;
  float best   = -1e9f;
  uint8_t callers = 0;
  for(uint8_t i = 0; i < d.n; i++){
    const AdamZone& z = d.zones[i];
    if(isnan(z.temp) || isnan(z.sp)) continue;
    float deficit = z.sp - z.temp;
    bool calling = (z.vraagt >= 0) ? (z.vraagt == 1) : (deficit > ADAM_CALL_DREMPEL);
    if(!calling) continue;
    callers++;
    // grootste deficit wint; gelijk → lagere cfg_idx (volgorde in config)
    if(deficit > best + 0.001f ||
       (fabsf(deficit - best) <= 0.001f && leader >= 0 &&
        z.cfg_idx < d.zones[leader].cfg_idx)){
      best = deficit; leader = i;
    }
  }
  callers_out = callers;
  return leader;
}

// ═══════════════════════════════════════════════════════════════
//  Publieke API
// ═══════════════════════════════════════════════════════════════
void adam_init(const char* ip, const char* pass,
               const char* const* zones, uint8_t zone_count){
  s_ip = ip; s_pass = pass;
  s_zone_count = (zone_count > ADAM_MAX_ZONES) ? ADAM_MAX_ZONES : zone_count;
  for(uint8_t i = 0; i < s_zone_count; i++) s_zones[i] = zones[i];
  s_init = true;
  s_status = "uit";
  Serial.print("[Adam] init: "); Serial.print(s_ip);
  Serial.print("  zones="); Serial.println(s_zone_count);
}

void adam_poll(){
  if(!s_init) return;
  if(bron != Bron::ADAM || modus != Modus::FF_WATER) return;

  static uint32_t vorige_ms = 0;
  uint32_t nu = millis();
  if(nu - vorige_ms < ADAM_POLL_MS) return;
  if(jgc_ontvangend()) return;        // niet mid-frame de lijn blokkeren
  vorige_ms = nu;

  AdamData d;
  bool ok = adam_fetch(d);

  if(!ok){
    s_status = "fout";
    s_fout_teller++;
    adam_leer_emitter_ok = false;      // bij twijfel niet leren
    if(s_fout_teller == 3)
      mqtt_log("Adam: 3x geen data — bron blijft adam, waarden bevroren", "WARNING");
    return;
  }

  s_status = "ok";
  s_laatste_ok_ms = nu;
  s_fout_teller = 0;

  // Water-setpoint (mirror van de MQTT-validatie)
  if(!isnan(d.water_sp)){
    if(d.water_sp == 0.0f)                      t_water_gewenst = 0.0f;
    else if(d.water_sp >= WATER_SP_MIN && d.water_sp <= 55.0f)
                                                t_water_gewenst = d.water_sp;
  }

  // Buitentemperatuur uit Adam — overschrijft de pomp-sensor in adam-modus.
  // De JGC-parser laat t_outside met rust zolang bron==ADAM (zie protocol.cpp).
  if(!isnan(d.outside) && d.outside > -40.0f && d.outside < 50.0f){
    t_outside = d.outside;
  }

  // Leidende zone → t_kamer / t_kamer_gewenst
  uint8_t callers = 0;
  int leader = kies_leider(d, callers);
  if(leader >= 0){
    const AdamZone& z = d.zones[leader];
    if(!isnan(z.temp)) t_kamer         = z.temp;
    if(!isnan(z.sp))   t_kamer_gewenst = z.sp;
    strncpy(s_leider, z.naam, sizeof(s_leider) - 1);
    s_leider[sizeof(s_leider)-1] = '\0';

    // Leer-gate: alleen leren bij stabiele, unieke leider
    if(z.cfg_idx != s_prev_leader){ s_prev_leader = z.cfg_idx; s_leider_sinds_ms = nu; }
    bool stabiel = (nu - s_leider_sinds_ms >= ADAM_LEAD_STABIEL_MS) && (callers == 1);
    adam_leer_emitter_ok = stabiel;
  } else {
    // geen vrager: geen warmtevraag, niet leren
    strncpy(s_leider, "-", sizeof(s_leider));
    s_prev_leader = -1;
    adam_leer_emitter_ok = false;
  }
}

bool        adam_beschikbaar(){ return s_init; }
const char* adam_status_str(){  return s_status; }
const char* adam_leider_naam(){ return s_leider; }
uint32_t    adam_ms_sinds_ok(){ return s_laatste_ok_ms ? (millis() - s_laatste_ok_ms) : 0; }
