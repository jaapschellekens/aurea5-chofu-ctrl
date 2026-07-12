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
static const uint16_t LEES_BUDGET     = 2048;    // bytes per loop()-doorgang (~2-4ms CPU)
static const uint32_t ZONE_STILTE_B   = 16384;   // geen <location meer na zoveel bytes → klaar

static WiFiClient adam_client;
static float      adam_sp_val      = NAN;
static float      adam_zone_val    = NAN;   // temperatuur van ADAM_ZONE
static uint32_t   laatste_ok_ms    = 0;
static uint32_t   laatste_poll_ms  = 0;
static uint32_t   poll_start_ms    = 0;
static uint8_t    fout_teller      = 0;
static bool       ooit_ok          = false;
static char       laatste_fout[40] = "";      // reden laatste mislukte poll
static char       http_status[4]   = "";      // HTTP-statuscode (bv. "401")
static uint8_t    http_idx         = 0;

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
  SCAN,                            // zoekt SP-type én <location parallel
  SP_GT, SP_VAL,                   // intended_boiler_temperature
  LOC_NAME, NAME_VAL,              // <location> → <name>
  ZONE_SCAN,                       // binnen zone: temp/thermostat/volgende locatie
  TEMP_GT, TEMP_VAL,               // <type>temperature</type> → <measurement>
  THERMO_GT, THERMO_VAL            // <type>thermostat</type> → <measurement>/<setpoint>
};
static AS as = AS::IDLE;

static Matcher m_hdr, m_sp, m_meas, m_loc, m_name, m_temp, m_thermo, m_setp, m_loc2;
static char  valbuf[16];
static uint8_t val_idx = 0;
static char  naam_buf[21];
static uint8_t naam_idx = 0;
static float sp_tmp = NAN;
static uint32_t bytes_sinds_loc = 0;   // bytes sinds laatste <location (voor vroege exit)
static uint32_t rx_bytes = 0;          // totaal ontvangen bytes deze poll (diagnose)

static void adam_reset_matchers(){
  m_hdr.init("\r\n\r\n");
  m_sp.init("<type>intended_boiler_temperature</type>");
  m_meas.init("<measurement");
  m_loc.init("<location ");
  m_loc2.init("<location ");
  m_name.init("<name>");
  m_temp.init("<type>temperature</type>");
  m_thermo.init("<type>thermostat</type>");
  m_setp.init("<setpoint>");
}

// Zone-in-opbouw (gecommit bij volgende <location of einde stream)
static float ztmp_temp = NAN, ztmp_sp = NAN;

static void zone_commit(){
  if(naam_buf[0] && !isnan(ztmp_temp) && ztmp_temp > -30 && ztmp_temp < 50
     && adam_zone_count < ADAM_MAX_ZONES){
    strncpy(adam_zones[adam_zone_count].naam, naam_buf, sizeof(naam_buf));
    adam_zones[adam_zone_count].temp = ztmp_temp;
    adam_zones[adam_zone_count].sp   = ztmp_sp;
    adam_zone_count++;
  }
  naam_buf[0] = 0;
  ztmp_temp = NAN; ztmp_sp = NAN;
}

static void zone_commit();  // forward

static void poll_afronden(bool ok, const char* reden = ""){
  adam_client.stop();
  as = AS::IDLE;
  if(!ok){
    strncpy(laatste_fout, reden, sizeof(laatste_fout) - 1);
    laatste_fout[sizeof(laatste_fout) - 1] = 0;
    mqtt_log(String("Adam poll mislukt: ") + reden, "WARNING");
  } else {
    laatste_fout[0] = 0;
  }
  if(ok){
    if(proto_logging)
      mqtt_log("Adam poll ok: " + String(rx_bytes) + "B in " +
               String(millis() - poll_start_ms) + "ms, " + String(adam_zone_count) + " zones", "INFO");
    zone_commit();  // laatste zone (stream-einde) nog committen
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
    case AdamBron::MULTI:       return "multi";
    default:                    return "uit";
  }
}

static char multi_actieve_zone[21] = "";
const char* adam_actieve_zone(){ return multi_actieve_zone; }
const char* adam_status_str(){
  if(!adam_actief()) return "uit";
  if(adam_geldig()) return "ok";
  // Nog nooit een geslaagde poll: dat is opstarten, geen fout — tenzij er
  // al een echte pollfout geregistreerd is.
  if(!ooit_ok && laatste_fout[0] == 0) return "init";
  return "fout";
}

// Directe poll afdwingen (bv. na bronwissel): interval-wachttijd overslaan
void adam_forceer_poll(){
  laatste_poll_ms = millis() - ADAM_POLL_MS;
  fout_teller = 0;          // backoff resetten
  laatste_fout[0] = 0;      // oude foutmelding wissen
}
const char* adam_fout_str(){ return laatste_fout; }
const char* adam_ip_str(){ return cfg_ip; }

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
      poll_afronden(false, "verbinding mislukt");
      return;
    }
    adam_client.println("GET /core/domain_objects HTTP/1.1");
    adam_client.print("Host: "); adam_client.println(cfg_ip);
    adam_client.println("Authorization: Basic " + adam_auth_b64());
    adam_client.println("Connection: close");
    adam_client.println();

    adam_reset_matchers();
    http_status[0] = 0; http_idx = 0;
    bytes_sinds_loc = 0;
    rx_bytes = 0;
    sp_tmp = NAN;
    adam_zone_count = 0;
    naam_buf[0] = 0;
    ztmp_temp = NAN; ztmp_sp = NAN;
    as = AS::HEADERS;
    return;
  }

  // Harde timeout per poll — met diagnose: hoever kwam de stream?
  if(nu - poll_start_ms > ADAM_TIMEOUT_MS){
    String d = "timeout " + String(rx_bytes) + "B st" + String((int)as);
    if(http_status[0]){ d += " http"; d += http_status; }
    poll_afronden(false, d.c_str());
    return;
  }

  // Verbinding weg en buffer leeg → afronden met wat we hebben
  if(!adam_client.available()){
    if(!adam_client.connected()){
      bool data_ok = !isnan(sp_tmp) || adam_zone_count > 0 || !isnan(ztmp_temp);
      if(data_ok) poll_afronden(true);
      else if(http_status[0] && strcmp(http_status, "200") != 0)
        poll_afronden(false, (String("http ") + http_status).c_str());
      else
        poll_afronden(false, "geen data in antwoord");
    }
    return;
  }

  // Begrensd budget verwerken
  uint16_t budget = LEES_BUDGET;
  while(budget-- && adam_client.available()){
    char c = (char)adam_client.read();
    rx_bytes++;

    // Vroege exit: de locaties staan aaneengesloten in de XML. Hebben we de SP
    // en minstens één zone, en komt er 16KB lang geen nieuwe <location, dan
    // volgt er niets bruikbaars meer — de resterende ~170KB overslaan scheelt
    // seconden per poll (en voorkomt de 20s-timeout).
    if(as == AS::SCAN || as == AS::ZONE_SCAN){
      if(++bytes_sinds_loc > ZONE_STILTE_B && !isnan(sp_tmp) && adam_zone_count > 0){
        zone_commit();
        poll_afronden(true);
        return;
      }
    }

    switch(as){

      case AS::HEADERS:
        // Statuscode uit "HTTP/1.1 XXX ..." (posities 9-11)
        if(http_idx < 12){
          if(http_idx >= 9 && http_idx <= 11) http_status[http_idx - 9] = c;
          if(http_idx == 11) http_status[3] = 0;
          http_idx++;
        }
        if(m_hdr.feed(c)) as = AS::SCAN;
        break;

      // ── SCAN: intended_boiler_temperature én <location parallel ──
      case AS::SCAN:
        if(isnan(sp_tmp) && m_sp.feed(c)){ m_meas.idx = 0; as = AS::SP_GT; }
        else if(m_loc.feed(c)){
          if(adam_zone_count >= ADAM_MAX_ZONES){ poll_afronden(true); return; }
          bytes_sinds_loc = 0;
          m_name.idx = 0; as = AS::LOC_NAME;
        }
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
          m_loc.idx = 0; m_sp.idx = 0;
          as = AS::SCAN;
        } else valbuf[val_idx++] = c;
        break;

      // ── zones: <location> → <name> → temperatuur/setpoint ────
      case AS::LOC_NAME: {
        // Appliances bevatten self-closing referenties (<location id="..."/>)
        // vóór de echte <location>-blokken — die overslaan, anders vult de
        // zonetabel zich met appliance-namen.
        static char vorige_c = 0;
        if(vorige_c == '/' && c == '>'){ vorige_c = 0; m_loc.idx = 0; as = AS::SCAN; break; }
        vorige_c = c;
        if(m_name.feed(c)){ vorige_c = 0; naam_idx = 0; as = AS::NAME_VAL; }
        break;
      }
      case AS::NAME_VAL:
        if(c == '<' || naam_idx >= sizeof(naam_buf) - 1){
          naam_buf[naam_idx] = 0;
          ztmp_temp = NAN; ztmp_sp = NAN;
          m_temp.idx = 0; m_thermo.idx = 0; m_loc2.idx = 0;
          as = AS::ZONE_SCAN;
        } else naam_buf[naam_idx++] = c;
        break;

      case AS::ZONE_SCAN:
        // Binnen het zoneblok: temperatuur, thermostaatsetpoint, of volgende zone
        if(m_temp.feed(c)){ m_meas.idx = 0; as = AS::TEMP_GT; }
        else if(m_thermo.feed(c)){ m_meas.idx = 0; m_setp.idx = 0; as = AS::THERMO_GT; }
        else if(m_loc2.feed(c)){
          zone_commit();
          if(adam_zone_count >= ADAM_MAX_ZONES){ poll_afronden(true); return; }
          bytes_sinds_loc = 0;
          m_name.idx = 0; as = AS::LOC_NAME;
        }
        break;

      case AS::TEMP_GT:
        if(m_meas.len != m_meas.idx){
          if(m_meas.feed(c)){ m_meas.idx = m_meas.len; }
          else if(m_loc2.feed(c)){ zone_commit(); m_name.idx = 0; as = AS::LOC_NAME; }
        } else if(c == '>'){ val_idx = 0; as = AS::TEMP_VAL; }
        break;
      case AS::TEMP_VAL:
        if(c == '<' || val_idx >= sizeof(valbuf) - 1){
          valbuf[val_idx] = 0;
          ztmp_temp = atof(valbuf);
          m_temp.idx = 0; m_thermo.idx = 0; m_loc2.idx = 0;
          as = AS::ZONE_SCAN;
        } else valbuf[val_idx++] = c;
        break;

      case AS::THERMO_GT:
        // Setpoint kan in een point_log (<measurement>) of in een
        // thermostat_functionality (<setpoint>) staan — beide accepteren
        if(m_meas.len != m_meas.idx){
          if(m_meas.feed(c)){ m_meas.idx = m_meas.len; break; }
          if(m_setp.feed(c)){ val_idx = 0; as = AS::THERMO_VAL; break; }
          if(m_loc2.feed(c)){ zone_commit(); m_name.idx = 0; as = AS::LOC_NAME; }
        } else if(c == '>'){ val_idx = 0; as = AS::THERMO_VAL; }
        break;
      case AS::THERMO_VAL:
        if(c == '<' || val_idx >= sizeof(valbuf) - 1){
          valbuf[val_idx] = 0;
          float v = atof(valbuf);
          if(v >= 4 && v <= 30) ztmp_sp = v;   // Anna/Tom setpointbereik
          m_temp.idx = 0; m_thermo.idx = 0; m_loc2.idx = 0;
          as = AS::ZONE_SCAN;
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

  // ── Watersetpoint (alleen water/ff_water, en alleen bij geldige SP) ──
  if(!isnan(sp) && (modus == Modus::WATER || modus == Modus::FF_WATER)){
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

  // ── Multi-zone: regel op de zone met de grootste vraag ───────
  // vraag = setpoint − temp (alleen zones met thermostaatsetpoint).
  // Wissel-hysterese 0.3°C voorkomt gependel tussen bijna-gelijke zones.
  // t_kamer_gewenst komt dan van de zone-thermostaat (Anna-setpoint genegeerd).
  if(adam_bron == AdamBron::MULTI){
    static bool multi_fout_gemeld = false;
    int besti = -1, acti = -1;
    float best_vraag = -1e9f, act_vraag = -1e9f;
    for(uint8_t i = 0; i < adam_zone_count; i++){
      const AdamZone& z = adam_zones[i];
      if(isnan(z.sp) || z.temp < 5 || z.temp > 35) continue;
      float vraag = z.sp - z.temp;
      if(vraag > best_vraag){ best_vraag = vraag; besti = i; }
      if(multi_actieve_zone[0] && strcmp(z.naam, multi_actieve_zone) == 0){
        acti = i; act_vraag = vraag;
      }
    }
    if(besti < 0){
      if(!multi_fout_gemeld){
        stuur_alert("Adam multi: geen zone met thermostaatsetpoint gevonden");
        multi_fout_gemeld = true;
      }
    } else {
      multi_fout_gemeld = false;
      // Actieve zone wisselen alleen bij >0.3C meer vraag (of geen actieve)
      if(acti < 0 || (besti != acti && best_vraag > act_vraag + 0.3f)){
        if(acti >= 0 || multi_actieve_zone[0] == 0)
          mqtt_log(String("Adam multi: actieve zone -> ") + adam_zones[besti].naam +
                   " (vraag " + String(best_vraag,1) + "C)", "INFO");
        strncpy(multi_actieve_zone, adam_zones[besti].naam, sizeof(multi_actieve_zone));
        acti = besti;
      }
      t_kamer         = adam_zones[acti].temp;
      t_kamer_gewenst = adam_zones[acti].sp;
      kamer_geldig    = true;
    }
  }
}
