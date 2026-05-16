#include "protocol.h"
#include "mqtt.h"   // voor stuur_alert()

uint8_t bereken_checksum(uint8_t *buf, uint8_t len){
  uint16_t sum = 0;
  for(uint8_t i=0; i<len; i++) sum += buf[i];
  return (sum & 0xFF);
}

static void stuur_stand_telegram_klassiek(){
  uint8_t telegram[25] = {0};
  telegram[0] = 0x19;
  telegram[1] = ctrl.stand;
  telegram[2] = 0x00;
  telegram[3] = (ctrl.stand == 0) ? 0 : (koeling_modus ? 2 : 1);
  telegram[23] = bereken_checksum(telegram, 23);
  telegram[24] = 0x00;
  chofuSerial.write(telegram, 25);
  String dec_tx = " | stand=" + String(ctrl.stand)
                + " modus=" + (ctrl.stand == 0 ? "uit" : (koeling_modus ? "koeling" : "verwarming"));
  mqtt_proto("tx", telegram, 25, dec_tx);
  Serial.print("TX: Stand ");Serial.print(ctrl.stand);Serial.println(" naar WP");
}

void verwerk_telegram_0x91(){
  if(telegram_buffer[0] != 0x91) return;
  uint8_t calc_cs = bereken_checksum(telegram_buffer, 23);
  if(calc_cs != telegram_buffer[23]){
    proto_crc_fouten++;
    String err = " | CS FOUT: berekend=" + String(calc_cs, HEX) + " ontvangen=" + String(telegram_buffer[23], HEX);
    mqtt_proto("err", telegram_buffer, 25, err);
    Serial.println("RX: checksum fout"); return;
  }

  int16_t temp_raw = (telegram_buffer[3] << 8) | telegram_buffer[4];
  float new_supply = temp_raw / 10.0;
  if(abs(new_supply - prev_t_supply) > 10.0) stuur_alert("Spike aanvoer: " + String(new_supply,1));
  else { t_supply = new_supply; prev_t_supply = new_supply; }

  temp_raw = (telegram_buffer[5] << 8) | telegram_buffer[6];
  float new_return = temp_raw / 10.0;
  if(abs(new_return - prev_t_return) > 10.0) stuur_alert("Spike retour: " + String(new_return,1));
  else { t_return = new_return; prev_t_return = new_return; }

  temp_raw = (telegram_buffer[7] << 8) | telegram_buffer[8];
  float new_outside = temp_raw / 10.0;
  if(new_outside < -30.0 || new_outside > 50.0) stuur_alert("Ongeldige buitentemp: " + String(new_outside,1));
  else if(abs(new_outside - prev_t_outside) > 5.0) stuur_alert("Spike buiten: " + String(new_outside,1));
  else { t_outside = new_outside; prev_t_outside = new_outside; }

  comp_hz = telegram_buffer[9];
  pomp_snelheid_wp = telegram_buffer[10];
  defrost = (telegram_buffer[11] & 0x01);

  if(comp_hz > 0){
    werkelijk_vermogen_w = 240 + ((comp_hz - 30) / 90.0) * 1210;
    if(werkelijk_vermogen_w < 0) werkelijk_vermogen_w = 0;
    if(werkelijk_vermogen_w > 1450) werkelijk_vermogen_w = 1450;
  } else {
    werkelijk_vermogen_w = 0;
  }
  delta_t = t_supply - t_return;

  // Protocol logging: hex + decoded betekenis
  String dec_rx = " | A=" + String(t_supply, 1)
                + " R=" + String(t_return, 1)
                + " B=" + String(t_outside, 1)
                + " Hz=" + String(comp_hz)
                + " P=" + String(pomp_snelheid_wp) + "%"
                + " ontd=" + String(defrost)
                + " b11=" + String(telegram_buffer[11], HEX)
                + " b12=" + String(telegram_buffer[12], HEX)
                + " b13=" + String(telegram_buffer[13], HEX)
                + " b14=" + String(telegram_buffer[14], HEX)
                + " b15=" + String(telegram_buffer[15], HEX);
  mqtt_proto("rx", telegram_buffer, 25, dec_rx);

  if(comp_hz > 0){
    werkelijk_vermogen_w = 240 + ((comp_hz - 30) / 90.0) * 1210;
    if(werkelijk_vermogen_w < 0) werkelijk_vermogen_w = 0;
    if(werkelijk_vermogen_w > 1450) werkelijk_vermogen_w = 1450;
  } else {
    werkelijk_vermogen_w = 0;
  }
  delta_t = t_supply - t_return;

  static uint8_t debug_count = 0;
  if(debug_count++ % 10 == 0){
    Serial.print("RX WP: A:");Serial.print(t_supply,1);
    Serial.print(" R:");Serial.print(t_return,1);
    Serial.print(" B:");Serial.print(t_outside,1);
    Serial.print(" Hz:");Serial.print(comp_hz);
    Serial.print(" P:");Serial.print(pomp_snelheid_wp);Serial.println("%");
  }
}

static void lees_warmtepomp_data_klassiek(){
  while(chofuSerial.available()){
    uint8_t byte = chofuSerial.read();
    if(byte == 0x91 || byte == 0x19){
      buffer_index = 0;
      telegram_buffer[buffer_index++] = byte;
    } else if(buffer_index > 0 && buffer_index < 25){
      telegram_buffer[buffer_index++] = byte;
      if(buffer_index == 25){
        if(telegram_buffer[0] == 0x91) verwerk_telegram_0x91();
        buffer_index = 0;
        vorige_telegram_ms = millis();
      }
    }
  }
  if(millis() - vorige_telegram_ms > 5000){
    if(proto_logging) mqtt_log("WP timeout: geen 0x91 >5s, stuur TX", "WARNING");
    stuur_stand_telegram_klassiek();
    vorige_telegram_ms = millis();
  }
}

// ═══════════════════════════════════════════════════════════════
//  JGC PARSER — multi-frame, CRC-CCITT, variabele frame-lengte
// ═══════════════════════════════════════════════════════════════

static const uint8_t JGC_VALID_LENGTHS[] = {13, 14, 19, 21};

static uint16_t jgc_bereken_crc(uint8_t *data, uint8_t len){
  uint16_t crc = 0xFFFF;
  for(uint8_t i = 0; i < len; i++){
    crc ^= (uint16_t)data[i] << 8;
    for(uint8_t j = 0; j < 8; j++){
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
  }
  return crc;
}

static bool jgc_geldige_lengte(uint8_t len){
  for(uint8_t i = 0; i < sizeof(JGC_VALID_LENGTHS); i++)
    if(len == JGC_VALID_LENGTHS[i]) return true;
  return false;
}

// Per-ID opgeslagen framedata (max 80 bytes, 4 IDs)
static uint8_t jgc_frames[4][80] = {0};

static void jgc_sla_frame_op(uint8_t id, uint8_t data_len, uint8_t *payload){
  // Bouw frame voor CRC-check: [0x91][id][data_len+2][payload...]
  uint8_t frame[80];
  frame[0] = 0x91;
  frame[1] = id;
  frame[2] = data_len + 2;
  for(uint8_t i = 0; i < data_len; i++) frame[3 + i] = payload[i];
  uint16_t crc = jgc_bereken_crc(frame, data_len + 2);
  if(crc != 0){
    proto_crc_fouten++;
    if(proto_logging) mqtt_log("JGC CRC fout ID=" + String(id), "WARNING");
    return;
  }
  jgc_frames[id][0] = 0x91;
  jgc_frames[id][1] = id;
  jgc_frames[id][2] = data_len + 2;
  for(uint8_t i = 0; i < data_len; i++) jgc_frames[id][i + 3] = payload[i];
}

static void jgc_verwerk_frames(){
  // Temperaturen uit ID=2: bytes [3][4]=aanvoer, [5][6]=retour, [7][8]=buiten
  // Gesorteerd als little-endian signed 16-bit, schaal 0.1°C
  auto lees_temp = [](uint8_t id, uint8_t lo_idx) -> float {
    int16_t raw = (int16_t)((uint16_t)jgc_frames[id][lo_idx + 1] << 8 | jgc_frames[id][lo_idx]);
    return raw / 10.0f;
  };

  float new_supply  = lees_temp(2, 3);
  float new_return  = lees_temp(2, 5);
  float new_outside = lees_temp(2, 7);

  if(abs(new_supply  - prev_t_supply)  > 10.0) stuur_alert("JGC spike aanvoer: "  + String(new_supply, 1));
  else { t_supply  = new_supply;  prev_t_supply  = new_supply; }

  if(abs(new_return  - prev_t_return)  > 10.0) stuur_alert("JGC spike retour: "   + String(new_return, 1));
  else { t_return  = new_return;  prev_t_return  = new_return; }

  if(new_outside < -30.0 || new_outside > 50.0) stuur_alert("JGC ongeldige buitentemp: " + String(new_outside, 1));
  else if(abs(new_outside - prev_t_outside) > 5.0) stuur_alert("JGC spike buiten: " + String(new_outside, 1));
  else { t_outside = new_outside; prev_t_outside = new_outside; }

  // Compressor Hz en pompsnelheid uit ID=3
  comp_hz          = jgc_frames[3][9];
  pomp_snelheid_wp = jgc_frames[3][10];

  // Defrost uit ID=1
  defrost = (jgc_frames[1][4] != 0);

  delta_t = t_supply - t_return;

  if(proto_logging){
    mqtt_log("JGC RX: A=" + String(t_supply,1) + " R=" + String(t_return,1)
           + " B=" + String(t_outside,1) + " Hz=" + String(comp_hz)
           + " P=" + String(pomp_snelheid_wp) + " ontd=" + String(defrost), "INFO");
  }
}

enum class JgcState : uint8_t { WachtStart, LeesHeader, LeesPayload, LeesEinde };
static JgcState jgc_state    = JgcState::WachtStart;
static uint8_t  jgc_id       = 0;
static uint8_t  jgc_data_len = 0;
static uint8_t  jgc_buf[80]  = {0};
static uint8_t  jgc_idx      = 0;

// Timing — identiek aan JGC origineel
static const uint32_t JGC_SEND_DELAY       =   99;  // ms na einde ontvangen frame
static const uint32_t JGC_SEND_TIMEOUT     = 2000;  // ms zonder reply → toch sturen
static const uint32_t JGC_MIN_SEND_INTERVAL=  300;  // ms minimale tijd tussen twee sends
static uint32_t jgc_laatste_rx_einde_ms    = 0;     // tijdstip einde laatste ontvangen frame
static uint32_t jgc_laatste_send_ms        = 0;     // tijdstip laatste TX
static bool     jgc_is_ontvangend          = false; // blokkeer TX tijdens ontvangst

static void lees_warmtepomp_data_jgc(){
  while(chofuSerial.available()){
    jgc_is_ontvangend = true;
    uint8_t b = chofuSerial.read();
    switch(jgc_state){
      case JgcState::WachtStart:
        if(b == 0x91){ jgc_state = JgcState::LeesHeader; jgc_idx = 0; }
        break;
      case JgcState::LeesHeader:
        if(jgc_idx == 0){
          jgc_id = b;
          if(jgc_id > 3){ jgc_state = JgcState::WachtStart; break; }
        } else {
          uint8_t msg_len = b + 1;
          if(!jgc_geldige_lengte(msg_len)){ jgc_state = JgcState::WachtStart; break; }
          jgc_data_len = msg_len - 3;
          jgc_idx = 0;
          jgc_state = JgcState::LeesPayload;
          break;
        }
        jgc_idx++;
        break;
      case JgcState::LeesPayload:
        jgc_buf[jgc_idx++] = b;
        if(jgc_idx >= jgc_data_len) jgc_state = JgcState::LeesEinde;
        break;
      case JgcState::LeesEinde:
        if(b == 0x00){
          jgc_sla_frame_op(jgc_id, jgc_data_len, jgc_buf);
          if(jgc_id == 2) jgc_verwerk_frames();
          vorige_telegram_ms = millis();
          jgc_laatste_rx_einde_ms = millis();
        } else {
          if(proto_logging) mqtt_log("JGC: eindnul ontbreekt ID=" + String(jgc_id), "WARNING");
        }
        jgc_is_ontvangend = false;
        jgc_state = JgcState::WachtStart;
        break;
    }
  }
  // Als de buffer leeg is zijn we niet meer aan het ontvangen
  if(!chofuSerial.available()) jgc_is_ontvangend = false;

  uint32_t nu = millis();
  bool na_delay   = (nu - jgc_laatste_rx_einde_ms >= JGC_SEND_DELAY) && !jgc_is_ontvangend;
  bool timeout    = (nu - jgc_laatste_rx_einde_ms >= JGC_SEND_TIMEOUT);
  bool min_interval_ok = (nu - jgc_laatste_send_ms >= JGC_MIN_SEND_INTERVAL);

  if((na_delay || timeout) && min_interval_ok){
    if(timeout && proto_logging) mqtt_log("JGC timeout: geen frame >2s, stuur TX", "WARNING");
    stuur_stand_telegram_jgc();
    jgc_laatste_send_ms = nu;
    vorige_telegram_ms  = nu;
  }
}

// JGC TX — roteert 4 telegrammen, data2 bevat stand + CRC-CCITT
static uint8_t jgc_tx0[] = { 0x19, 0x0, 0x8, 0x0, 0x0, 0x0, 0xd9, 0xb5 };
static uint8_t jgc_tx1[] = { 0x19, 0x1, 0x0c, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xaa, 0x35 };
static uint8_t jgc_tx2[] = { 0x19, 0x2, 0x8, 0x1, 0x1, 0x0, 0x99, 0x37 };
static uint8_t jgc_tx3[] = { 0x19, 0x3, 0x8, 0xb2, 0x2, 0x0, 0xc1, 0x9a };
static uint8_t jgc_telegram_count = 0;

static void stuur_stand_telegram_jgc(){
  switch(jgc_telegram_count){
    case 0:
      chofuSerial.write(jgc_tx0, sizeof(jgc_tx0));
      if(proto_logging) mqtt_proto("tx-jgc0", jgc_tx0, sizeof(jgc_tx0), " | init0");
      break;
    case 1:
      chofuSerial.write(jgc_tx1, sizeof(jgc_tx1));
      if(proto_logging) mqtt_proto("tx-jgc1", jgc_tx1, sizeof(jgc_tx1), " | init1");
      break;
    case 2: {
      uint8_t speed = ctrl.stand;
      jgc_tx2[3] = (speed == 0) ? 0x00 : 0x01;
      jgc_tx2[4] = speed;
      uint16_t crc = jgc_bereken_crc(jgc_tx2, sizeof(jgc_tx2) - 2);
      jgc_tx2[6] = (crc >> 8) & 0xFF;
      jgc_tx2[7] = crc & 0xFF;
      chofuSerial.write(jgc_tx2, sizeof(jgc_tx2));
      String dec = " | stand=" + String(speed) + " modus=" + (speed == 0 ? "uit" : (koeling_modus ? "koeling" : "verwarming"));
      if(proto_logging) mqtt_proto("tx-jgc2", jgc_tx2, sizeof(jgc_tx2), dec);
      break;
    }
    case 3:
      chofuSerial.write(jgc_tx3, sizeof(jgc_tx3));
      if(proto_logging) mqtt_proto("tx-jgc3", jgc_tx3, sizeof(jgc_tx3), " | init3");
      break;
  }
  jgc_telegram_count = (jgc_telegram_count + 1) % 4;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIEKE DISPATCHERS
// ═══════════════════════════════════════════════════════════════

void stuur_stand_telegram(){
  if(parser_jgc) stuur_stand_telegram_jgc();
  else           stuur_stand_telegram_klassiek();
}

void lees_warmtepomp_data(){
  if(parser_jgc) lees_warmtepomp_data_jgc();
  else           lees_warmtepomp_data_klassiek();
}

void pas_sim_toe(){
  if(!sim_enabled) return;
  if(!isnan(sim_t_supply))        { t_supply        = sim_t_supply;        prev_t_supply  = sim_t_supply; }
  if(!isnan(sim_t_return))        { t_return        = sim_t_return;        prev_t_return  = sim_t_return; }
  if(!isnan(sim_t_outside))       { t_outside       = sim_t_outside;       prev_t_outside = sim_t_outside; }
  if(!isnan(sim_t_water_gewenst)) { t_water_gewenst = sim_t_water_gewenst; }
  if(!isnan(sim_t_kamer))         { t_kamer         = sim_t_kamer; }
  if(!isnan(sim_t_kamer_gewenst)) { t_kamer_gewenst = sim_t_kamer_gewenst; }
  if(sim_actief()) delta_t = t_supply - t_return;
}
