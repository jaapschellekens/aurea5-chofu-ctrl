#include "adam.h"
#include "mqtt.h"

// ═══════════════════════════════════════════════════════════════
//  PLUGWISE ADAM — niet-blokkerende HTTP/XML-poll
//
//  KRITIEK: dit bestand mag NOOIT blokkeren. De JGC-pomptiming
//  (TX ≤ 99 ms na frame-einde) verdraagt geen wachtlussen. Alles
//  loopt als state machine met een klein leesbudget per doorgang.
// ═══════════════════════════════════════════════════════════════

AdamBron adam_bron    = AdamBron::UIT;
float    ADAM_KOEL_SP = 18.0f;

// Credentials — gezet via adam_config() vanuit setup() (config.h is alleen
// zichtbaar in de .ino; hier includen zou dubbele definities geven)
static const char* cfg_ip   = "";
static const char* cfg_pass = "";
static const char* cfg_zone = "";

void adam_config(const char* ip, const char* pass, const char* zone){
  cfg_ip = ip; cfg_pass = pass; cfg_zone = zone;
}

AdamZone adam_zones[ADAM_MAX_ZONES];
uint8_t  adam_zone_count = 0;

static const uint32_t ADAM_POLL_MS    = 30000;   // interval
static const uint32_t ADAM_TIMEOUT_MS = 20000;   // harde limiet per poll
static const uint32_t ADAM_BACKOFF_MS = 300000;  // 5 min na herhaald falen
static const uint32_t ADAM_GELDIG_MS  = 90000;   // data vervalt na 90s
static const uint16_t LEES_BUDGET     = 512;     // bytes per loop()-doorgang

static WiFiClient adam_client;
static float      adam_sp_val      = NAN;
static float      adam_zone_val    = NAN;   // temperatuur van ADAM_ZONE
static uint32_t   laatste_ok_ms    = 0;
static uint32_t   laatste_poll_ms  = 0;
static uint32_t   poll_start_ms    = 0;
static uint8_t    fout_teller      = 0;
static bool       ooit_ok          = false;

// ── Herstartbare substring-matcher ─────────────────────────────
struct Matcher {
  const char* pat;
  uint8_t len, idx;
  void init(const char* p){ pat = p; len = strlen(p); idx = 0; }
  bool feed(char c){
    if(c == pat[idx]){ if(++idx == len){ idx = 0; return true; } }
    else idx = (c == pat[0]) ? 1 : 0;
    return false;
  }
};

enum class AS : uint8_t {
  IDLE, HEADERS,
  ZOEK_SP, SP_GT, SP_VAL,          // intended_boiler_temperature
  ZOEK_LOC, LOC_NAME, NAME_VAL,    // <location> → <name>
  ZOEK_TEMP, TEMP_GT, TEMP_VAL     // <type>temperature</type> → <measurement>
};
static AS as = AS::IDLE;

static Matcher m_hdr, m_sp, m_meas, m_loc, m_name, m_temp, m_loc2;
static char  valbuf[16];
static uint8_t val_idx = 0;
static char  naam_buf[21];
static uint8_t naam_idx = 0;
static float sp_tmp = NAN;

static void adam_reset_matchers(){
  m_hdr.init("\r\n\r\n");
  m_sp.init("<type>intended_boiler_temperature</type>");
  m_meas.init("<measurement");
  m_loc.init("<location ");
  m_loc2.init("<location ");
  m_name.init("<name>");
  m_temp.init("<type>temperature</type>");
}

static void poll_afronden(bool ok){
  adam_client.stop();
  as = AS::IDLE;
  if(ok){
    adam_sp_val = sp_tmp;
    // Zone opzoeken voor t_kamer
    adam_zone_val = NAN;
    for(uint8_t i = 0; i < adam_zone_count; i++)
      if(strcmp(adam_zones[i].naam, cfg_zone) == 0){ adam_zone_val = adam_zones[i].temp; break; }
    laatste_ok_ms = millis();
    fout_teller = 0;
    ooit_ok = true;
  } else {
    if(fout_teller < 255) fout_teller++;
  }
}

// ── Publieke status ────────────────────────────────────────────
bool adam_actief(){ return adam_bron != AdamBron::UIT && cfg_ip[0] != 0; }
bool adam_geldig(){ return ooit_ok && (millis() - laatste_ok_ms) < ADAM_GELDIG_MS; }
float adam_water_sp(){ return adam_geldig() ? adam_sp_val : NAN; }
float adam_zone_temp(){ return adam_geldig() ? adam_zone_val : NAN; }

const char* adam_bron_str(){
  switch(adam_bron){
    case AdamBron::WATER:       return "water";
    case AdamBron::WATER_KAMER: return "water+kamer";
    default:                    return "uit";
  }
}
const char* adam_status_str(){
  if(!adam_actief()) return "uit";
  return adam_geldig() ? "ok" : "fout";
}

// ── Base64 (voor basic auth, credentials zijn compile-time) ───
static String adam_auth_b64(){
  static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String s = String("smile:") + cfg_pass;
  String out;
  const uint8_t* in = (const uint8_t*)s.c_str();
  int len = s.length();
  for(int i = 0; i < len; i += 3){
    uint8_t a = in[i], b = (i+1 < len) ? in[i+1] : 0, c = (i+2 < len) ? in[i+2] : 0;
    out += B64[a >> 2]; out += B64[((a & 3) << 4) | (b >> 4)];
    out += (i+1 < len) ? B64[((b & 0xF) << 2) | (c >> 6)] : '=';
    out += (i+2 < len) ? B64[c & 0x3F] : '=';
  }
  return out;
}

// ── Poll state machine ─────────────────────────────────────────
void adam_poll(){
  if(!adam_actief()){
    if(as != AS::IDLE){ adam_client.stop(); as = AS::IDLE; }
    return;
  }

  uint32_t nu = millis();

  // Nieuwe poll starten?
  if(as == AS::IDLE){
    uint32_t interval = (fout_teller >= 3) ? ADAM_BACKOFF_MS : ADAM_POLL_MS;
    if(nu - laatste_poll_ms < interval) return;
    laatste_poll_ms = nu;
    poll_start_ms   = nu;

    // connect() blokkeert kort (LAN: <100ms bij bereikbare host); bij
    // onbereikbare host beschermt de backoff tegen herhaald blokkeren.
    if(!adam_client.connect(cfg_ip, 80)){
      poll_afronden(false);
      return;
    }
    adam_client.println("GET /core/domain_objects HTTP/1.1");
    adam_client.print("Host: "); adam_client.println(cfg_ip);
    adam_client.println("Authorization: Basic " + adam_auth_b64());
    adam_client.println("Connection: close");
    adam_client.println();

    adam_reset_matchers();
    sp_tmp = NAN;
    adam_zone_count = 0;
    as = AS::HEADERS;
    return;
  }

  // Harde timeout per poll
  if(nu - poll_start_ms > ADAM_TIMEOUT_MS){
    poll_afronden(false);
    return;
  }

  // Verbinding weg en buffer leeg → afronden met wat we hebben
  if(!adam_client.available()){
    if(!adam_client.connected()) poll_afronden(!isnan(sp_tmp));
    return;
  }

  // Begrensd budget verwerken
  uint16_t budget = LEES_BUDGET;
  while(budget-- && adam_client.available()){
    char c = (char)adam_client.read();
    switch(as){

      case AS::HEADERS:
        if(m_hdr.feed(c)) as = AS::ZOEK_SP;
        break;

      // ── intended_boiler_temperature ──────────────────────────
      case AS::ZOEK_SP:
        if(m_sp.feed(c)){ m_meas.idx = 0; as = AS::SP_GT; }
        break;
      case AS::SP_GT:
        // eerst "<measurement" vinden, daarna '>' van de open-tag
        if(m_meas.len != m_meas.idx){
          if(m_meas.feed(c)){ /* gevonden, nu wachten op '>' */ m_meas.idx = m_meas.len; }
        } else if(c == '>'){ val_idx = 0; as = AS::SP_VAL; }
        break;
      case AS::SP_VAL:
        if(c == '<' || val_idx >= sizeof(valbuf) - 1){
          valbuf[val_idx] = 0;
          sp_tmp = atof(valbuf);
          m_loc.idx = 0;
          as = AS::ZOEK_LOC;
        } else valbuf[val_idx++] = c;
        break;

      // ── zones: <location> → <name> → temperature ─────────────
      case AS::ZOEK_LOC:
        if(adam_zone_count >= ADAM_MAX_ZONES){ poll_afronden(true); return; }
        if(m_loc.feed(c)){ m_name.idx = 0; as = AS::LOC_NAME; }
        break;
      case AS::LOC_NAME: {
        // Appliances bevatten self-closing referenties (<location id="..."/>)
        // vóór de echte <location>-blokken — die overslaan, anders vult de
        // zonetabel zich met appliance-namen.
        static char vorige_c = 0;
        if(vorige_c == '/' && c == '>'){ vorige_c = 0; m_loc.idx = 0; as = AS::ZOEK_LOC; break; }
        vorige_c = c;
        if(m_name.feed(c)){ vorige_c = 0; naam_idx = 0; as = AS::NAME_VAL; }
        break;
      }
      case AS::NAME_VAL:
        if(c == '<' || naam_idx >= sizeof(naam_buf) - 1){
          naam_buf[naam_idx] = 0;
          m_temp.idx = 0; m_loc2.idx = 0;
          as = AS::ZOEK_TEMP;
        } else naam_buf[naam_idx++] = c;
        break;
      case AS::ZOEK_TEMP:
        // temperatuur binnen dit blok, óf volgend <location = zone zonder sensor
        if(m_temp.feed(c)){ m_meas.idx = 0; as = AS::TEMP_GT; }
        else if(m_loc2.feed(c)){ m_name.idx = 0; as = AS::LOC_NAME; }
        break;
      case AS::TEMP_GT:
        if(m_meas.len != m_meas.idx){
          if(m_meas.feed(c)){ m_meas.idx = m_meas.len; }
        } else if(c == '>'){ val_idx = 0; as = AS::TEMP_VAL; }
        break;
      case AS::TEMP_VAL:
        if(c == '<' || val_idx >= sizeof(valbuf) - 1){
          valbuf[val_idx] = 0;
          float t = atof(valbuf);
          if(t > -30 && t < 50 && adam_zone_count < ADAM_MAX_ZONES){
            strncpy(adam_zones[adam_zone_count].naam, naam_buf, sizeof(naam_buf));
            adam_zones[adam_zone_count].temp = t;
            adam_zone_count++;
          }
          m_loc.idx = 0;
          as = AS::ZOEK_LOC;
        } else valbuf[val_idx++] = c;
        break;

      default: break;
    }
  }
}

// ── Toepassen op de regeling ───────────────────────────────────
// Aanroepen in loop() vóór pas_sim_toe() (simulator wint altijd).
void adam_pas_toe(){
  static bool  uitval_gemeld = false;
  static float vorige_sp     = NAN;

  if(!adam_actief()) { uitval_gemeld = false; return; }

  if(!adam_geldig()){
    if(ooit_ok && !uitval_gemeld){
      stuur_alert("Adam API onbereikbaar - terug naar MQTT-bron");
      uitval_gemeld = true;
    }
    return;  // laatste MQTT-waarden blijven gelden
  }
  if(uitval_gemeld){
    mqtt_log("Adam API hersteld", "INFO");
    uitval_gemeld = false;
  }

  float sp = adam_sp_val;
  bool nieuw = isnan(vorige_sp) || fabs(sp - vorige_sp) > 0.05f;

  // ── Watersetpoint (alleen water/ff_water) ────────────────────
  if(modus == Modus::WATER || modus == Modus::FF_WATER){
    if(sp == 0.0f){
      // Geen vraag; koeling laten zoals hij is (SP=0 komt ook voor tussen koelcycli)
      t_water_gewenst = 0.0f;
      if(nieuw) mqtt_log("Adam SP: 0 (geen vraag)", "INFO");
    }
    else if(sp > 9.5f && sp < 10.5f){
      // Anna geeft vast 10°C door als koelvraag
      if(modus == Modus::FF_WATER){
        if(!koeling_modus){
          koeling_modus = true; ctrl.reset_pid();
          mqtt_log("Adam: koelvraag (SP=10) - koeling aan", "INFO");
        }
        t_water_gewenst = max(ADAM_KOEL_SP, SUPPLY_MIN);
      } else if(nieuw){
        stuur_alert("Adam koelvraag genegeerd: koeling vereist ff_water");
      }
    }
    else if(sp >= 16.0f && sp <= 55.0f){
      if(koeling_modus){
        koeling_modus = false; ctrl.reset_pid();
        mqtt_log("Adam: verwarmingsvraag - koeling uit", "INFO");
      }
      t_water_gewenst = sp;
      if(nieuw) mqtt_log("Adam SP: " + String(sp,1) + "C", "INFO");
    }
    else if(nieuw){
      mqtt_log("Adam SP " + String(sp,1) + "C buiten bereik - genegeerd", "WARNING");
    }
  }
  vorige_sp = sp;

  // ── Kamertemperatuur uit geconfigureerde zone ────────────────
  if(adam_bron == AdamBron::WATER_KAMER){
    static bool zone_fout_gemeld = false;
    float tz = adam_zone_val;
    if(!isnan(tz) && tz >= 5 && tz <= 35){
      t_kamer = tz;
      kamer_geldig = true;
      zone_fout_gemeld = false;
    } else if(!zone_fout_gemeld){
      stuur_alert("Adam zone '" + String(cfg_zone) + "' niet gevonden - kamerbron blijft Anna/MQTT");
      zone_fout_gemeld = true;
    }
  }
}
