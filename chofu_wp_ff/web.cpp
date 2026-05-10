#include "web.h"
#include "eeprom.h"  // voor eeprom_save()

void handle_web_client(){
  if(millis() - vorige_web_check_ms < 100) return;
  vorige_web_check_ms = millis();
  WiFiClient client = webServer.available();
  if(!client) return;
  Serial.println("Web client verbonden");
  String request = "";
  uint32_t web_timeout = millis();
  while(client.connected() && millis() - web_timeout < 2000){
    if(client.available()){
      char c = client.read(); request += c;
      if(c == '\n' && request.endsWith("\r\n\r\n")) break;
    }
  }
  if(!client.connected() && request.length() == 0){ client.stop(); return; }
  if(request.indexOf("GET /?") >= 0){
    auto parse_param = [&](const char* key) -> String {
      String k = String(key) + "=";
      int idx = request.indexOf(k);
      if(idx < 0) return "";
      idx += k.length();
      int end1 = request.indexOf("&", idx);
      int end2 = request.indexOf(" ", idx);
      int end = (end1 > 0 && end1 < end2) ? end1 : end2;
      return request.substring(idx, end);
    };
    String v;
    v = parse_param("setpoint");   if(v.length()){ setpoint = v.toFloat(); eeprom_save(); }
    v = parse_param("kp");         if(v.length()){ Kp = v.toFloat(); eeprom_save(); }
    v = parse_param("ki");         if(v.length()){ Ki = v.toFloat(); eeprom_save(); }
    v = parse_param("kd");         if(v.length()){ Kd = v.toFloat(); eeprom_save(); }
    v = parse_param("kp_water");   if(v.length()){ Kp_water = v.toFloat(); eeprom_save(); }
    v = parse_param("ki_water");   if(v.length()){ Ki_water = v.toFloat(); eeprom_save(); }
    v = parse_param("kd_water");   if(v.length()){ Kd_water = v.toFloat(); eeprom_save(); }
    v = parse_param("modus");      if(v.length() && (v=="auto"||v=="water"||v=="ff_auto"||v=="ff_water"||v=="handmatig")){
      modus = str_naar_modus(v); if(modus != Modus::HANDMATIG){ ctrl.reset_pid(); ctrl.reset_ff(); } }
    v = parse_param("water_setpoint"); if(v.length()){ float f=v.toFloat(); if(f>=25&&f<=55) t_water_gewenst=f; }
    v = parse_param("stooklijn_aan");  if(v.length()){ float f=v.toFloat(); if(f>=0&&f<=25&&f<STOOKLIJN_UIT_GRENS){ STOOKLIJN_AAN_GRENS=f; eeprom_save(); } }
    v = parse_param("ff_ua_house");    if(v.length()){ float f=v.toFloat(); if(f>=50&&f<=500){ ff_UA_house=f; eeprom_save(); } }
    v = parse_param("ff_ua_emitter");  if(v.length()){ float f=v.toFloat(); if(f>=50&&f<=500){ ff_UA_emitter=f; eeprom_save(); } }
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println();
  client.println("<!DOCTYPE html><html><head>");
  client.println("<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  client.println("<title>Kromhout WP v2.0</title>");
  client.println("<style>body{font-family:Arial;margin:20px;background:#f0f0f0}h1{color:#2c3e50}.card{background:white;padding:20px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}.temp{font-size:24px;font-weight:bold;color:#e74c3c}input,select{padding:8px;margin:5px;border:1px solid #ccc;border-radius:4px}button{background:#3498db;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer}button:hover{background:#2980b9}.status{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:5px}.on{background:#27ae60}.off{background:#95a5a6}.ff{background:#8e44ad}</style></head><body>");
  client.println("<h1>Kromhout Warmtepomp v2.0</h1>");

  // Status
  client.println("<div class='card'><h2>Status</h2>");
  client.print("<div><span class='status "); client.print(ctrl.wp_aan?"on":"off"); client.print("'></span>WP: <b>"); client.print(ctrl.wp_aan?"AAN":"UIT"); client.println("</b></div>");
  client.print("<div>Modus: <b>"); client.print(modus_naar_str(modus)); client.println("</b></div>");
  client.print("<div>Stand: <b>"); client.print(ctrl.stand); client.print("</b> ("); client.print(VERMOGEN[ctrl.stand]); client.println(" W)</div>");
  if(modus == Modus::FF_AUTO || modus == Modus::FF_WATER){
    client.print("<div>FF UA huis: <b>"); client.print(ff_UA_house,0); client.println(" W/K</b></div>");
    client.print("<div>FF UA emitter: <b>"); client.print(ff_UA_emitter,0); client.println(" W/K</b></div>");
  }
  client.println("</div>");

  // Temperaturen
  client.println("<div class='card'><h2>Temperaturen</h2>");
  client.print("<div>Aanvoer: <span class='temp'>"); client.print(t_supply,1); client.println("°C</span></div>");
  client.print("<div>Retour: <span class='temp'>"); client.print(t_return,1); client.println("°C</span></div>");
  client.print("<div>Delta T: <span class='temp'>"); client.print(delta_t,1); client.println("°C</span></div>");
  client.print("<div>Buiten: <span class='temp'>"); client.print(t_outside,1); client.println("°C</span></div>");
  client.print("<div>Kamer: <span class='temp'>"); client.print(t_kamer,1); client.print("°C</span> → "); client.print(t_kamer_gewenst,1); client.println("°C</div>");
  client.println("</div>");

  // Instellingen
  client.println("<div class='card'><h2>Instellingen</h2><form>");
  client.print("<div>Setpoint: <input type='number' name='setpoint' value='"); client.print(setpoint,1); client.println("' step='0.1' min='20' max='45'> °C</div>");
  client.print("<div>Modus: <select name='modus'>");
  for(const char* m : {"auto","water","ff_auto","ff_water","handmatig"}){
    client.print("<option value='"); client.print(m); client.print("'");
    if(strcmp(modus_naar_str(modus), m) == 0) client.print(" selected");
    client.print(">"); client.print(m); client.println("</option>");
  }
  client.println("</select></div>");
  client.print("<div>Water setpoint: <input type='number' name='water_setpoint' value='"); client.print(t_water_gewenst,1); client.println("' step='0.1' min='25' max='55'> °C</div>");
  client.print("<div>Stooklijn aan (&lt; uit): <input type='number' name='stooklijn_aan' value='"); client.print(STOOKLIJN_AAN_GRENS,1); client.println("' step='0.5' min='0' max='25'> °C</div>");
  client.println("<h3>PID Parameters — AUTO modus</h3>");
  client.print("<div>Kp: <input type='number' name='kp' value='"); client.print(Kp,2); client.println("' step='0.5' min='0' max='500'></div>");
  client.print("<div>Ki: <input type='number' name='ki' value='"); client.print(Ki,4); client.println("' step='0.001' min='0' max='5'></div>");
  client.print("<div>Kd: <input type='number' name='kd' value='"); client.print(Kd,4); client.println("' step='0.001' min='0' max='50'></div>");
  client.println("<h3>PID Parameters — WATER modus</h3>");
  client.print("<div>Kp water: <input type='number' name='kp_water' value='"); client.print(Kp_water,2); client.println("' step='0.5' min='0' max='500'></div>");
  client.print("<div>Ki water: <input type='number' name='ki_water' value='"); client.print(Ki_water,4); client.println("' step='0.001' min='0' max='5'></div>");
  client.print("<div>Kd water: <input type='number' name='kd_water' value='"); client.print(Kd_water,4); client.println("' step='0.001' min='0' max='50'></div>");
  client.println("<h3>FF Parameters (lerende UA)</h3>");
  client.print("<div>UA huis: <input type='number' name='ff_ua_house' value='"); client.print(ff_UA_house,0); client.println("' step='1' min='50' max='500'> W/K</div>");
  client.print("<div>UA emitter: <input type='number' name='ff_ua_emitter' value='"); client.print(ff_UA_emitter,0); client.println("' step='1' min='50' max='500'> W/K</div>");
  client.print("<div>PID output: <b>"); client.print(ctrl.pid_output,1); client.println("%</b></div>");
  client.println("<br><button type='submit'>Opslaan</button></form></div>");

  client.print("<div class='card'><small>IP: "); client.print(WiFi.localIP());
  client.print(" | Uptime: "); client.print(millis()/1000/60); client.println(" min</small></div>");
  client.println("<script>setTimeout(function(){location.reload()},10000);</script>");
  client.println("</body></html>");
  delay(10);
  client.stop();
}
