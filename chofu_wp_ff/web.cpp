#include "web.h"
#include "eeprom.h"  // voor eeprom_save()
#include "mqtt.h"    // voor set_param()

// Server-side tabs (geen JavaScript): de actieve tab zit in ?tab=. Per request
// wordt alleen die ene tab gerenderd (licht voor de MCU). Opslaan loopt via de
// gedeelde set_param() zodat web en MQTT exact dezelfde validatie gebruiken;
// daarom zijn de formulier-veldnamen gelijk aan de MQTT-cmd-segmenten.
// Onder elk veld staat een korte uitleg (wat het doet + gevolg van aanpassen).

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
    ".heat{font-size:22px;font-weight:bold;color:#e74c3c}"
    ".cool{font-size:22px;font-weight:bold;color:#2980b9}"
    ".ext{font-size:22px;font-weight:bold;color:#111}"
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
    const char* tcls = koeling_modus ? "cool" : "heat";   // Chofu-metingen: rood=verwarmen, blauw=koelen
    client.println("<div class='card'><h2>Temperaturen</h2>");
    client.print("<div>Aanvoer: <span class='"); client.print(tcls); client.print("'>"); client.print(t_supply,1); client.println("°C</span></div>");
    client.print("<div>Retour: <span class='"); client.print(tcls); client.print("'>"); client.print(t_return,1); client.println("°C</span></div>");
    client.print("<div>Delta T: "); client.print(delta_t,1); client.println("°C</div>");
    client.print("<div>Doel-aanvoer: "); client.print(doel_setpoint,1); client.println("°C</div>");
    client.print("<div>Buiten: <span class='"); client.print(tcls); client.print("'>"); client.print(t_outside,1); client.println("°C</span></div>");
    client.print("<div>Kamer (extern): <span class='ext'>"); client.print(t_kamer,1); client.print("°C</span> → "); client.print(t_kamer_gewenst,1); client.println("°C</div>");
    client.println("<small class='hint'>Rood/blauw = meting van de Chofu (verwarmen/koelen); zwart = extern gezet.</small>");
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
    client.print("</select><small class='hint'>Bepaalt de regelstrategie: AUTO=kamer-PID, WATER=aanvoer-PID op vast setpoint, FF_*=feedforward (zelflerend), handmatig=vaste stand.</small></div>");
    onoff("koeling","Koeling", koeling_modus, "Aan = WP koelt (aanvoer kouder dan kamer) i.p.v. verwarmt. Werkt alleen in FF_AUTO/FF_WATER/handmatig.");
    onoff("sww","Tapwater laden (SWW)", sww_actief, "Aan = onderbreekt verwarming en laadt het tapwatervat; klep schakelt om, koeling gaat uit.");
    fend();
    // Handmatige stand — apart formulier (zet modus automatisch op handmatig).
    client.println("<div class='card'><h2>Handmatige stand</h2><form action='/'><input type='hidden' name='tab' value='bediening'>");
    client.print("<div>Stand: <input type='number' name='stand' value='"); client.print(handmatig_stand);
    client.print("' step='1' min='0' max='12'><small class='hint'>Vaste compressorstand 0–12: hoger = meer vermogen/warmte. Zet de modus automatisch op handmatig.</small></div>");
    client.println("<button type='submit'>Zet stand</button></form></div>");
    // losse actie
    client.println("<div class='card'><form action='/'><input type='hidden' name='tab' value='bediening'>");
    client.println("<button type='submit' name='force_start' value='1'>Force start</button>");
    client.println("<small class='hint'>Reset de hysterese-timer zodat de WP meteen mag bijschakelen i.p.v. de wachttijd af te wachten.</small></form></div>");
  }
  else if(tab == "stooklijn"){
    fstart("stooklijn","Stooklijn");
    num("stooklijn_basis","Basis-aanvoer", String(setpoint,1), "0.5","20","45","°C",
        "Aanvoertemp bij mild weer. Hoger = warmer huis maar minder zuinig (lagere COP).");
    num("stooklijn_grens","Curve start", String(STOOKLIJN_GRENS,1), "0.5","0","25","°C",
        "Buitentemp waaronder de aanvoer omhoog gaat. Hoger = de curve begint al bij milder weer.");
    num("stooklijn_factor","Helling", String(STOOKLIJN_FACTOR,2), "0.05","0.1","5","°C/°C",
        "Steilheid. Hoger = aanvoer stijgt sneller bij kou (warmer bij vorst); lager = vlakker en zuiniger.");
    num("stooklijn_uit","Seizoensstop", String(STOOKLIJN_UIT_GRENS,1), "0.5","5","30","°C",
        "Boven deze buitentemp stopt de verwarming. Lager = WP stopt eerder in het voorjaar.");
    num("stooklijn_aan","Hervat", String(STOOKLIJN_AAN_GRENS,1), "0.5","0","25","°C",
        "Onder deze buitentemp hervat de verwarming. Het gat met 'stop' is hysterese tegen aan/uit-pendelen.");
    num("t_vorst","Vorstgrens", String(T_VORST,1), "0.5","-10","10","°C",
        "Onder deze buitentemp draait de WP altijd min. stand 1. Hoger = eerder vorstbeveiliging (veiliger, minder zuinig).");
    fend();
  }
  else if(tab == "water"){
    fstart("water","Water & kamer");
    num("water_setpoint","Water setpoint", String(t_water_gewenst,1), "0.5","0","55","°C",
        "Gewenste aanvoertemp (WATER/FF_WATER). Hoger = warmer/sneller maar lagere COP. 0 = geen vraag → WP uit.");
    num("water_sp_min","Water SP min", String(WATER_SP_MIN,1), "0.5","10","30","°C",
        "Laagste geldige water-setpoint; waarden 1..min-1 worden genegeerd (beschermt tegen onzin-lage waarden).");
    onoff("kamer_in_water","Kamertemp in water-modi", kamer_in_water,
        "Aan = kamertemp corrigeert mee in water-modi; uit = puur op aanvoertemp regelen.");
    num("kamer","Kamertemp (extern)", String(t_kamer,1), "0.1","5","35","°C",
        "Externe kamertemperatuur. Een hogere gemeten waarde → WP denkt dat het warmer is → minder vermogen.");
    num("kamer_setpoint","Kamer setpoint", String(t_kamer_gewenst,1), "0.5","14","30","°C",
        "Gewenste kamertemperatuur. Hoger = WP regelt naar een hogere temp (meer verbruik).");
    fend();
  }
  else if(tab == "pid"){
    fstart("pid","PID");
    client.println("<h3>AUTO modus</h3>");
    num("kp","Kp", String(Kp,2), "0.5","0.1","500","", "Proportioneel (AUTO). Hoger = reageert sneller maar schakelt meer / meer overshoot.");
    num("ki","Ki", String(Ki,4), "0.001","0","5","", "Integraal (AUTO). Hoger = restafwijking sneller weg, maar risico op slingeren.");
    num("kd","Kd", String(Kd,4), "0.001","0","50","", "Differentieel (AUTO). Hoger = dempt overshoot, maar gevoeliger voor ruis.");
    client.println("<h3>WATER modus</h3>");
    num("kp_water","Kp water", String(Kp_water,2), "0.5","0.1","500","", "Proportioneel voor aanvoer-tracking (WATER). Hoger = sneller, meer schakelen.");
    num("ki_water","Ki water", String(Ki_water,4), "0.001","0","5","", "Integraal (WATER). Hoger = restafwijking sneller weg, risico op slingeren.");
    num("kd_water","Kd water", String(Kd_water,4), "0.001","0","50","", "Differentieel (WATER). Hoger = dempt overshoot, gevoeliger voor ruis.");
    fend();
  }
  else if(tab == "ff"){
    fstart("ff","Feedforward (lerende UA)");
    num("ff_ua_house","UA huis", String(ff_UA_house,0), "1","50","500","W/K",
        "Warmteverlies huis. Hoger = WP rekent op meer vraag → kiest een hogere stand. Leert online; stuur dit bij comfortklachten in FF_AUTO.");
    num("ff_ua_emitter","UA emitter", String(ff_UA_emitter,0), "1","50","500","W/K",
        "Afgifte radiatoren/vloer. Hoger = model vindt een lagere aanvoer al voldoende. Leert online in FF_WATER.");
    fend();
    client.println("<div class='card'><form action='/'><input type='hidden' name='tab' value='ff'>");
    client.println("<button type='submit' name='ff_save' value='1'>Leerwaarden opslaan</button>");
    client.println("<small class='hint'>Slaat de online geleerde UA-waarden op in EEPROM zodat ze een herstart overleven.</small></form></div>");
  }
  else if(tab == "koeling"){
    fstart("koeling","Koeling");
    onoff("koeling","Koeling actief", koeling_modus,
        "Aan = koelen i.p.v. verwarmen (alleen FF_AUTO/FF_WATER/handmatig).");
    num("koeling_min_buiten","Min. buitentemp", String(KOELING_MIN_BUITEN,1), "0.5","0","30","°C",
        "Onder deze buitentemp stopt koeling. Hoger = koeling alleen bij echt warm weer.");
    num("supply_min","Min. aanvoer", String(SUPPLY_MIN,1), "0.5","10","25","°C",
        "Laagste aanvoertemp (dauwpunt). Hoger = veiliger tegen condens; lager = meer koelvermogen.");
    fend();
  }
  else if(tab == "sww"){
    fstart("sww","Tapwater (SWW)");
    onoff("sww","Tapwater laden", sww_actief,
        "Aan = start tapwater laden: driewegklep schakelt, koeling uit, eigen setpoint/stand.");
    num("sww_setpoint","SWW setpoint", String(SWW_SETPOINT,1), "0.5","30","60","°C",
        "Aanvoertemp tijdens laden. Hoger = heter tapwater/sneller klaar, maar lagere COP.");
    num("sww_max_stand","SWW max stand", String(SWW_MAX_STAND), "1","1","8","",
        "Max stand tijdens SWW. Hoger = sneller laden (meer vermogen/geluid). Los van de algemene max stand.");
    fend();
  }
  else if(tab == "grenzen"){
    fstart("grenzen","Grenzen & systeem");
    num("supply_max","Max aanvoer", String(SUPPLY_MAX,1), "1","40","80","°C",
        "Veiligheidsplafond: bij overschrijding volgt noodstop. Niet hoger zetten dan de installatie aankan.");
    num("max_stand","Max stand", String(MAX_STAND), "1","1","8","",
        "Hoogste compressorstand (alle modi behalve handmatig/SWW). Lager = stiller/zuiniger maar trager opwarmen.");
    onoff("lcd","LCD", lcd_enabled, "Display aan/uit.");
    onoff("proto_log","Protocol logging", proto_logging, "Aan = ruwe JGC-telegrammen naar MQTT (debug; meer verkeer).");
    onoff("seriallog","Serial-log naar MQTT", seriallog_enabled, "Aan = logregels gebatcht naar MQTT (debug).");
    fend();
  }
  else if(tab == "geavanceerd"){
    fstart("geavanceerd","Geavanceerd");
    num("pid_interval","PID interval", String(pid_interval_ms), "100","100","60000","ms",
        "Hoe vaak de stand herberekend wordt. Lager = sneller reageren maar meer schakelen/last.");
    num("hyst_slow","Hyst traag", String(HYST_SLOW_MS), "1000","100","3600000","ms",
        "Min. tijd tussen standverhogingen bij kleine fout. Hoger = rustiger, trager opbouwen.");
    num("hyst_fast","Hyst snel", String(HYST_FAST_MS), "1000","100","3600000","ms",
        "Idem bij grote fout. Lager = sneller opschalen als de afwijking groot is.");
    num("hyst_down","Hyst afbouw", String(HYST_DOWN_MS), "1000","100","3600000","ms",
        "Min. tijd voordat teruggeschakeld wordt. Hoger = minder pendelen, maar kan overshoot geven.");
    num("auto_hyst_down","Auto afbouw", String(AUTO_HYST_DOWN_MS/60000.0f,1), "0.5","0","30","min",
        "Trager afbouwen in AUTO. Hoger = WP blijft langer op stand staan.");
    num("ff_min_off","FF min uit", String(FF_MIN_OFF_MS/60000.0f,1), "1","0","120","min",
        "Min. uitschakelperiode na een stop. Hoger = minder kort-cyclen, maar trager herstarten.");
    num("ff_restart_coast","FF herstart coast", String(FF_RESTART_COAST,2), "0.05","0","5","°C",
        "Hoeveel onder setpoint nodig om vanuit stand 0 te herstarten. Hoger = WP wacht langer met herstarten.");
    num("ff_lookahead","FF vooruitkijk", String(FF_LOOKAHEAD_MS/60000.0f,1), "0.5","0","60","min",
        "Vooruitkijktijd predictieve terugschakeling. Hoger = schakelt eerder terug (anticipeert meer); 0 = uit.");
    num("ff_thermal_min_off","FF thermisch min uit", String(FF_THERMAL_MIN_OFF_MS/60000.0f,1), "0.5","0","30","min",
        "Min. uit-tijd na een thermische stop (korter dan seizoensstop). Hoger = langere pauze.");
    num("ff_afschakel","FF afschakel (auto)", String(FF_AFSCHAKEL_AUTO,2), "0.1","-3","0","°C",
        "°C kamer boven setpoint waarbij FF begint terug te schakelen. Negatiever = later terugschakelen (warmer laten worden).");
    num("koeling_afschakel","Koeling afschakel", String(KOELING_AFSCHAKEL,2), "0.1","0.1","5","°C",
        "°C onder setpoint waarbij koeling terugschakelt. Hoger = schakelt eerder terug.");
    num("koel_deadband","Koel doodband", String(KOEL_DEADBAND,1), "0.1","0","5","°C",
        "Mismatch waarbinnen niet wordt opgeschaald. Hoger = rustiger, accepteert een grotere afwijking voordat het vermogen omhoog gaat.");
    fend();
  }

  // ── Voettekst ───────────────────────────────────────────────────
  client.print("<div class='card'><small>IP: "); client.print(WiFi.localIP());
  client.print(" | Uptime: "); client.print(millis()/1000/60); client.println(" min</small></div>");
  client.println("</body></html>");
  delay(10);
  client.stop();
}
