#include "web.h"
#include "eeprom.h"  // voor eeprom_save()
#include "mqtt.h"    // voor set_param()

// Server-side tabs (geen JavaScript): de actieve tab zit in ?tab=. Per request
// wordt alleen die ene tab gerenderd (licht voor de MCU). Opslaan loopt via de
// gedeelde set_param() zodat web en MQTT exact dezelfde validatie gebruiken;
// daarom zijn de formulier-veldnamen gelijk aan de MQTT-cmd-segmenten.
// Onder elk veld staat een korte uitleg (hint) uit de documentatie.

void handle_web_client(){
  if(millis() - vorige_web_check_ms < 100) return;
  vorige_web_check_ms = millis();
  WiFiClient client = webServer.available();
  if(!client) return;
  String request = "";
  uint32_t web_timeout = millis();
  while(client.connected() && millis() - web_timeout < 2000){
    if(client.available()){
      char c = client.read(); request += c;
      if(c == '\n' && request.endsWith("\r\n\r\n")) break;
    }
  }
  if(!client.connected() && request.length() == 0){ client.stop(); return; }

  // ── Query-string uitlezen ──────────────────────────────────────
  String qs;
  {
    int s = request.indexOf("/?");
    if(s >= 0){
      int e = request.indexOf(' ', s);
      if(e > s) qs = request.substring(s + 2, e);   // "a=1&b=2&tab=x"
    }
  }
  auto param = [&](const char* key) -> String {
    String k = String(key) + "=";
    int idx = qs.indexOf(k);
    if(idx < 0) return "";
    if(idx != 0 && qs.charAt(idx - 1) != '&') return "";
    idx += k.length();
    int amp = qs.indexOf('&', idx);
    return (amp < 0) ? qs.substring(idx) : qs.substring(idx, amp);
  };

  // ── Instellingen toepassen via gedeelde set_param() ─────────────
  bool gewijzigd = false;
  if(qs.length()){
    int start = 0;
    while(start < (int)qs.length()){
      int amp = qs.indexOf('&', start);
      if(amp < 0) amp = qs.length();
      String pair = qs.substring(start, amp);
      int eq = pair.indexOf('=');
      if(eq > 0){
        String key = pair.substring(0, eq);
        String val = pair.substring(eq + 1);
        if(key != "tab" && val.length()){ set_param(key, val); gewijzigd = true; }
      }
      start = amp + 1;
    }
  }
  if(gewijzigd) data_sturen_gevraagd = true;   // HA/MQTT laten meelopen

  // ── Actieve tab ─────────────────────────────────────────────────
  String tab = param("tab");
  if(tab.length() == 0) tab = "status";

  // ── HTTP + head ─────────────────────────────────────────────────
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  if(tab == "status") client.println("<meta http-equiv='refresh' content='30; url=/?tab=status'>");
  client.println("<title>Chofu WP</title>");
  client.println("<style>"
    "body{font-family:Arial;margin:16px;background:#f0f0f0}h1{color:#2c3e50;font-size:20px}"
    "h2{font-size:16px}h3{font-size:14px;color:#555;margin:14px 0 4px}"
    ".card{background:#fff;padding:16px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.1)}"
    ".temp{font-size:22px;font-weight:bold;color:#e74c3c}"
    "input,select{padding:7px;margin:4px;border:1px solid #ccc;border-radius:4px}"
    "button{background:#3498db;color:#fff;padding:9px 18px;border:none;border-radius:4px;cursor:pointer}"
    "button:hover{background:#2980b9}"
    "nav{margin:10px 0;line-height:2}"
    "nav a{display:inline-block;padding:7px 11px;margin:2px;background:#dde;border-radius:4px;text-decoration:none;color:#2c3e50;font-size:13px}"
    "nav a.active{background:#3498db;color:#fff}"
    ".hint{display:block;color:#888;font-size:11px;margin:1px 0 9px 6px}"
    ".st{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:5px}.on{background:#27ae60}.off{background:#95a5a6}"
    "</style></head><body>");
  client.println("<h1>Chofu Warmtepomp</h1>");

  // ── Navigatie ───────────────────────────────────────────────────
  auto nav = [&](const char* id, const char* label){
    client.print("<a href='/?tab="); client.print(id); client.print("'");
    if(tab == id) client.print(" class='active'");
    client.print(">"); client.print(label); client.print("</a>");
  };
  client.print("<nav>");
  nav("status","Status"); nav("bediening","Bediening"); nav("stooklijn","Stooklijn");
  nav("water","Water & kamer"); nav("pid","PID"); nav("ff","Feedforward");
  nav("koeling","Koeling"); nav("sww","Tapwater"); nav("grenzen","Grenzen & systeem");
  nav("geavanceerd","Geavanceerd");
  client.println("</nav>");

  // ── Hulp-lambda's ───────────────────────────────────────────────
  auto fstart = [&](const char* tabid, const char* titel){
    client.print("<div class='card'><h2>"); client.print(titel); client.print("</h2><form action='/'>");
    client.print("<input type='hidden' name='tab' value='"); client.print(tabid); client.println("'>");
  };
  auto fend = [&](){ client.println("<br><button type='submit'>Opslaan</button></form></div>"); };
  auto num = [&](const char* name, const char* label, const String& val, const char* step,
                 const char* mn, const char* mx, const char* unit, const char* hint){
    client.print("<div>"); client.print(label); client.print(": <input type='number' name='");
    client.print(name); client.print("' value='"); client.print(val);
    client.print("' step='"); client.print(step); client.print("' min='"); client.print(mn);
    client.print("' max='"); client.print(mx); client.print("'> "); client.print(unit);
    client.print("<small class='hint'>"); client.print(hint); client.println("</small></div>");
  };
  auto onoff = [&](const char* name, const char* label, bool on, const char* hint){
    client.print("<div>"); client.print(label); client.print(": <select name='"); client.print(name);
    client.print("'><option value='0'"); if(!on) client.print(" selected");
    client.print(">uit</option><option value='1'"); if(on) client.print(" selected");
    client.print(">aan</option></select><small class='hint'>"); client.print(hint); client.println("</small></div>");
  };

  // ── Tab-inhoud ──────────────────────────────────────────────────
  if(tab == "status"){
    client.println("<div class='card'><h2>Status</h2>");
    client.print("<div><span class='st "); client.print(ctrl.wp_aan?"on":"off"); client.print("'></span>WP: <b>"); client.print(ctrl.wp_aan?"AAN":"UIT"); client.println("</b></div>");
    client.print("<div>Modus: <b>"); client.print(modus_naar_str(modus)); client.println("</b></div>");
    client.print("<div>Stand: <b>"); client.print(ctrl.stand); client.print("</b> ("); client.print(VERMOGEN[ctrl.stand]); client.print(" W), max <b>"); client.print(MAX_STAND); client.println("</b></div>");
    client.print("<div>PID/FF output: <b>"); client.print(ctrl.pid_output,1); client.println("%</b></div>");
    if(koeling_modus) client.println("<div><span class='st on'></span>Koeling actief</div>");
    if(sww_actief){ client.print("<div><span class='st on'></span>SWW: tapwater laden → "); client.print(SWW_SETPOINT,1); client.println("°C</div>"); }
    if(modus == Modus::FF_AUTO || modus == Modus::FF_WATER){
      client.print("<div>FF UA huis: <b>"); client.print(ff_UA_house,0); client.print("</b> | emitter: <b>"); client.print(ff_UA_emitter,0); client.println("</b> W/K</div>");
    }
    client.println("</div>");
    client.println("<div class='card'><h2>Temperaturen</h2>");
    client.print("<div>Aanvoer: <span class='temp'>"); client.print(t_supply,1); client.println("°C</span></div>");
    client.print("<div>Retour: <span class='temp'>"); client.print(t_return,1); client.println("°C</span></div>");
    client.print("<div>Delta T: "); client.print(delta_t,1); client.println("°C</div>");
    client.print("<div>Doel-aanvoer: "); client.print(doel_setpoint,1); client.println("°C</div>");
    client.print("<div>Buiten: <span class='temp'>"); client.print(t_outside,1); client.println("°C</span></div>");
    client.print("<div>Kamer: <span class='temp'>"); client.print(t_kamer,1); client.print("°C</span> → "); client.print(t_kamer_gewenst,1); client.println("°C</div>");
    client.print("<div>Compressor: "); client.print(comp_hz); client.print(" Hz"); if(defrost) client.print(" | DEFROST"); client.println("</div>");
    client.println("</div>");
  }
  else if(tab == "bediening"){
    fstart("bediening","Bediening");
    client.print("<div>Modus: <select name='modus'>");
    for(const char* m : {"auto","water","ff_auto","ff_water","handmatig"}){
      client.print("<option value='"); client.print(m); client.print("'");
      if(strcmp(modus_naar_str(modus), m) == 0) client.print(" selected");
      client.print(">"); client.print(m); client.println("</option>");
    }
    client.print("</select><small class='hint'>AUTO=kamer-PID, WATER=aanvoer-PID, FF_*=feedforward, handmatig=vaste stand.</small></div>");
    onoff("koeling","Koeling", koeling_modus, "Koelen i.p.v. verwarmen (alleen FF_AUTO/FF_WATER/handmatig).");
    onoff("sww","Tapwater laden (SWW)", sww_actief, "Laadt het tapwatervat: schakelt de klep, eigen setpoint/stand, koeling uit.");
    fend();
    // Handmatige stand — apart formulier (zet modus automatisch op handmatig).
    client.println("<div class='card'><h2>Handmatige stand</h2><form action='/'><input type='hidden' name='tab' value='bediening'>");
    client.print("<div>Stand: <input type='number' name='stand' value='"); client.print(handmatig_stand);
    client.print("' step='1' min='0' max='12'><small class='hint'>Vaste compressorstand 0–12; zet de modus op handmatig.</small></div>");
    client.println("<button type='submit'>Zet stand</button></form></div>");
    // losse actie
    client.println("<div class='card'><form action='/'><input type='hidden' name='tab' value='bediening'>");
    client.println("<button type='submit' name='force_start' value='1'>Force start</button>");
    client.println("<small class='hint'>Reset de hysterese-timer zodat de WP direct mag bijschakelen.</small></form></div>");
  }
  else if(tab == "stooklijn"){
    fstart("stooklijn","Stooklijn");
    num("stooklijn_basis","Basis-aanvoer", String(setpoint,1), "0.5","20","45","°C",
        "Aanvoertemp bij mild weer (vlak deel van de curve, bij/boven de grens).");
    num("stooklijn_grens","Curve start", String(STOOKLIJN_GRENS,1), "0.5","0","25","°C",
        "Buitentemp waaronder de aanvoer omhoog gaat.");
    num("stooklijn_factor","Helling", String(STOOKLIJN_FACTOR,2), "0.05","0.1","5","°C/°C",
        "Hoeveel °C aanvoer extra per °C kouder buiten.");
    num("stooklijn_uit","Seizoensstop", String(STOOKLIJN_UIT_GRENS,1), "0.5","5","30","°C",
        "Boven deze buitentemp stopt de verwarming (AUTO/FF_AUTO).");
    num("stooklijn_aan","Hervat", String(STOOKLIJN_AAN_GRENS,1), "0.5","0","25","°C",
        "Onder deze buitentemp hervat de verwarming (hysterese t.o.v. stop).");
    num("t_vorst","Vorstgrens", String(T_VORST,1), "0.5","-10","10","°C",
        "Onder deze buitentemp draait de WP altijd min. stand 1 (vorstbeveiliging).");
    fend();
  }
  else if(tab == "water"){
    fstart("water","Water & kamer");
    num("water_setpoint","Water setpoint", String(t_water_gewenst,1), "0.5","0","55","°C",
        "Gewenste aanvoertemp (WATER/FF_WATER). 0 = geen warmtevraag → WP uit.");
    num("water_sp_min","Water SP min", String(WATER_SP_MIN,1), "0.5","10","30","°C",
        "Laagste geldige water-setpoint; waarden 1..min-1 worden genegeerd.");
    onoff("kamer_in_water","Kamertemp in water-modi", kamer_in_water,
        "Kamertemp meewegen in water-modi; uit = alleen op aanvoertemp regelen.");
    num("kamer","Kamertemp (extern)", String(t_kamer,1), "0.1","5","35","°C",
        "Externe kamertemperatuur (bv. zonder eigen sensor).");
    num("kamer_setpoint","Kamer setpoint", String(t_kamer_gewenst,1), "0.5","14","30","°C",
        "Gewenste kamertemperatuur.");
    fend();
  }
  else if(tab == "pid"){
    fstart("pid","PID");
    client.println("<h3>AUTO modus</h3>");
    num("kp","Kp", String(Kp,2), "0.5","0.1","500","", "Proportionele versterking (AUTO).");
    num("ki","Ki", String(Ki,4), "0.001","0","5","", "Integraal-versterking (AUTO).");
    num("kd","Kd", String(Kd,4), "0.001","0","50","", "Differentieel-versterking (AUTO).");
    client.println("<h3>WATER modus</h3>");
    num("kp_water","Kp water", String(Kp_water,2), "0.5","0.1","500","", "Proportioneel voor aanvoer-tracking (WATER).");
    num("ki_water","Ki water", String(Ki_water,4), "0.001","0","5","", "Integraal (WATER).");
    num("kd_water","Kd water", String(Kd_water,4), "0.001","0","50","", "Differentieel (WATER).");
    fend();
  }
  else if(tab == "ff"){
    fstart("ff","Feedforward (lerende UA)");
    num("ff_ua_house","UA huis", String(ff_UA_house,0), "1","50","500","W/K",
        "Warmteverlies huis; leert online. Stuur dit bij comfortklachten in FF_AUTO.");
    num("ff_ua_emitter","UA emitter", String(ff_UA_emitter,0), "1","50","500","W/K",
        "Warmteafgifte radiatoren/vloer; leert online in FF_WATER.");
    fend();
    client.println("<div class='card'><form action='/'><input type='hidden' name='tab' value='ff'>");
    client.println("<button type='submit' name='ff_save' value='1'>Leerwaarden opslaan</button>");
    client.println("<small class='hint'>Sla de geleerde UA-waarden op in EEPROM.</small></form></div>");
  }
  else if(tab == "koeling"){
    fstart("koeling","Koeling");
    onoff("koeling","Koeling actief", koeling_modus,
        "Koelen aan/uit (alleen FF_AUTO/FF_WATER/handmatig).");
    num("koeling_min_buiten","Min. buitentemp", String(KOELING_MIN_BUITEN,1), "0.5","0","30","°C",
        "Onder deze buitentemp stopt koeling (geen zin als het buiten koel is).");
    num("supply_min","Min. aanvoer", String(SUPPLY_MIN,1), "0.5","10","25","°C",
        "Laagste aanvoertemp — dauwpunt-/condensatiebescherming.");
    fend();
  }
  else if(tab == "sww"){
    fstart("sww","Tapwater (SWW)");
    onoff("sww","Tapwater laden", sww_actief,
        "Start/stop tapwater laden; schakelt de driewegklep en zet koeling uit.");
    num("sww_setpoint","SWW setpoint", String(SWW_SETPOINT,1), "0.5","30","60","°C",
        "Aanvoertemp tijdens het laden van het tapwatervat.");
    num("sww_max_stand","SWW max stand", String(SWW_MAX_STAND), "1","1","8","",
        "Max compressorstand tijdens SWW (los van de algemene max stand).");
    fend();
  }
  else if(tab == "grenzen"){
    fstart("grenzen","Grenzen & systeem");
    num("supply_max","Max aanvoer", String(SUPPLY_MAX,1), "1","40","80","°C",
        "Veiligheidsplafond: bij overschrijding noodstop (alle modi).");
    num("max_stand","Max stand", String(MAX_STAND), "1","1","8","",
        "Hoogste compressorstand in alle modi behalve handmatig (en SWW).");
    onoff("lcd","LCD", lcd_enabled, "Display aan/uit.");
    onoff("proto_log","Protocol logging", proto_logging, "Ruwe JGC-telegrammen naar MQTT (debug).");
    onoff("seriallog","Serial-log naar MQTT", seriallog_enabled, "Logregels gebatcht naar MQTT (debug).");
    fend();
  }
  else if(tab == "geavanceerd"){
    fstart("geavanceerd","Geavanceerd");
    num("pid_interval","PID interval", String(pid_interval_ms), "100","100","60000","ms",
        "Hoe vaak de regelaar de stand herberekent.");
    num("hyst_slow","Hyst traag", String(HYST_SLOW_MS), "1000","100","3600000","ms",
        "Min. tijd tussen standverhogingen bij kleine regelfout.");
    num("hyst_fast","Hyst snel", String(HYST_FAST_MS), "1000","100","3600000","ms",
        "Min. tijd tussen standverhogingen bij grote regelfout.");
    num("hyst_down","Hyst afbouw", String(HYST_DOWN_MS), "1000","100","3600000","ms",
        "Min. tijd voordat teruggeschakeld wordt.");
    num("auto_hyst_down","Auto afbouw", String(AUTO_HYST_DOWN_MS/60000.0f,1), "0.5","0","30","min",
        "Trager afbouwen in AUTO dan in WATER.");
    num("ff_min_off","FF min uit", String(FF_MIN_OFF_MS/60000.0f,1), "1","0","120","min",
        "Min. uitschakelperiode na een (seizoens)stop.");
    num("ff_restart_coast","FF herstart coast", String(FF_RESTART_COAST,2), "0.05","0","5","°C",
        "Hoeveel onder setpoint nodig om vanuit stand 0 te herstarten.");
    num("ff_lookahead","FF vooruitkijk", String(FF_LOOKAHEAD_MS/60000.0f,1), "0.5","0","60","min",
        "Vooruitkijktijd voor predictieve terugschakeling (0=uit).");
    num("ff_thermal_min_off","FF thermisch min uit", String(FF_THERMAL_MIN_OFF_MS/60000.0f,1), "0.5","0","30","min",
        "Min. uit-tijd na thermische stop (korter dan seizoensstop).");
    num("ff_afschakel","FF afschakel (auto)", String(FF_AFSCHAKEL_AUTO,2), "0.1","-3","0","°C",
        "°C kamer boven setpoint waarbij FF begint terug te schakelen.");
    num("koeling_afschakel","Koeling afschakel", String(KOELING_AFSCHAKEL,2), "0.1","0.1","5","°C",
        "°C onder setpoint waarbij koeling terugschakelt.");
    num("koel_deadband","Koel doodband", String(KOEL_DEADBAND,1), "0.1","0","5","°C",
        "Koel-mismatch waarbinnen niet wordt opgeschaald (kleine afwijking is ok).");
    fend();
  }

  // ── Voettekst ───────────────────────────────────────────────────
  client.print("<div class='card'><small>IP: "); client.print(WiFi.localIP());
  client.print(" | Uptime: "); client.print(millis()/1000/60); client.println(" min</small></div>");
  client.println("</body></html>");
  delay(10);
  client.stop();
}
