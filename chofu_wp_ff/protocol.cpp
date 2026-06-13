#include "protocol.h"
#include "mqtt.h"   // voor stuur_alert()

static void stuur_stand_telegram_jgc(); // forward declaration

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

// Per-ID opgeslagen framedata (max 21 bytes payload + 3 header = 24, afgerond naar 25)
static uint8_t jgc_frames[4][25] = {0};
static bool    jgc_rx_pending[4] = {false};  // frame wacht op uitgestelde hex-publicatie
static uint8_t jgc_rx_len[4]     = {0};
static uint16_t jgc_frame_aborts   = 0;   // frames afgebroken op mid-frame timeout
static uint32_t jgc_frames_ok      = 0;   // geldige frames (CRC OK)
static uint32_t jgc_laatste_byte_ms = 0;  // tijdstip laatste ontvangen byte
// Uitgestelde TX hex-logging (publish direct na write() stoort de RX van het antwoord)
static uint8_t jgc_tx_pending_buf[12];
static uint8_t jgc_tx_pending_len = 0;   // 0 = niets pending
static uint8_t jgc_tx_pending_nr  = 0;

static void jgc_sla_frame_op(uint8_t id, uint8_t data_len, uint8_t *payload){
  // data_len = msg_len - 4 = werkelijke payloadbytes (incl. 2 CRC-bytes aan het eind).
  // Bouw frame voor CRC-check: [0x91][id][lenbyte=data_len+3][payload...]
  // CRC-CCITT over het hele frame incl. CRC-bytes geeft residu 0.
  uint8_t frame[25];
  frame[0] = 0x91;
  frame[1] = id;
  frame[2] = data_len + 3;
  for(uint8_t i = 0; i < data_len; i++) frame[3 + i] = payload[i];
  uint16_t crc = jgc_bereken_crc(frame, data_len + 3);
  if(crc != 0){
    proto_crc_fouten++;  // geen mqtt_log hier: publish in RX-pad veroorzaakt nieuwe fouten (cascade)
    return;
  }
  jgc_frames_ok++;
  jgc_frames[id][0] = 0x91;
  jgc_frames[id][1] = id;
  jgc_frames[id][2] = data_len + 3;
  for(uint8_t i = 0; i < data_len; i++) jgc_frames[id][i + 3] = payload[i];
  // GEEN mqtt_proto hier: een blokkerende publish in het RX-pad verdringt de
  // Serial1-interrupt → byte-overrun → CRC-fouten op volgende frames.
  // Publicatie gebeurt uitgesteld in lees_warmtepomp_data_jgc().
  jgc_rx_pending[id] = true;
  jgc_rx_len[id] = data_len + 3;
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

  // Spike-guard: prev óók bij afwijzing bijwerken zodat een aanhoudende
  // nieuwe waarde bij het tweede frame geaccepteerd wordt.
  if(abs(new_supply  - prev_t_supply)  > 10.0) stuur_alert("JGC spike aanvoer: "  + String(new_supply, 1));
  else t_supply = new_supply;
  prev_t_supply = new_supply;

  if(abs(new_return  - prev_t_return)  > 10.0) stuur_alert("JGC spike retour: "   + String(new_return, 1));
  else t_return = new_return;
  prev_t_return = new_return;

  if(new_outside < -30.0 || new_outside > 50.0){
    stuur_alert("JGC ongeldige buitentemp: " + String(new_outside, 1));
  } else {
    if(abs(new_outside - prev_t_outside) > 5.0) stuur_alert("JGC spike buiten: " + String(new_outside, 1));
    else t_outside = new_outside;
    prev_t_outside = new_outside;
  }

  // Compressor Hz en pompsnelheid uit ID=3
  comp_hz          = jgc_frames[3][9];
  pomp_snelheid_wp = jgc_frames[3][10];

  // Defrost uit ID=1
  defrost = (jgc_frames[1][4] != 0);

  delta_t = t_supply - t_return;
  // Geen mqtt_log "JGC RX" meer hier: blokkerende publish in het RX-pad.
  // De gedecodeerde waarden gaan al elke 10s via stuur_data(), en de ruwe
  // frames via de uitgestelde hex-logging.
}

enum class JgcState : uint8_t { WachtStart, LeesHeader, LeesPayload };
static JgcState jgc_state    = JgcState::WachtStart;
static uint8_t  jgc_id       = 0;
static uint8_t  jgc_data_len = 0;
static uint8_t  jgc_buf[25]  = {0};
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
    jgc_laatste_byte_ms = millis();
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
          // Werkelijke payload op de draad = msg_len - 4 (header 3 + payload,
          // GEEN terminator). De "eindnul" uit jgc.ino is een AVR-artefact:
          // de lijn-release na het frame komt op een AVR-UART binnen als 0x00
          // (break), maar de Renesas-UART van de UNO R4 filtert die weg.
          // Wachten op een eindnul betekent op de R4 dus eeuwig wachten.
          jgc_data_len = msg_len - 4;
          jgc_idx = 0;
          jgc_state = JgcState::LeesPayload;
          break;
        }
        jgc_idx++;
        break;
      case JgcState::LeesPayload:
        jgc_buf[jgc_idx++] = b;
        if(jgc_idx >= jgc_data_len){
          // Frame compleet (CRC-bytes zitten in de payload, geen terminator)
          jgc_sla_frame_op(jgc_id, jgc_data_len, jgc_buf);
          if(jgc_id == 2) jgc_verwerk_frames();
          vorige_telegram_ms = millis();
          jgc_laatste_rx_einde_ms = millis();
          jgc_is_ontvangend = false;
          jgc_state = JgcState::WachtStart;
        }
        break;
    }
  }
  // BELANGRIJK (zie jgc.ino: IsReceiving komt pas vrij in Read_End):
  // 'ontvangend' blijft waar zolang de parser midden in een frame zit, óók als
  // de buffer even leeg is. De pomp pauzeert >99ms vóór de afsluitende 0x00;
  // een poll in die pauze botst met de eindnul (half-duplex lijn met echo) →
  // frame kapot én poll genegeerd. Dit was de oorzaak van ~100% RX-verlies.
  if(!chofuSerial.available() && jgc_state == JgcState::WachtStart)
    jgc_is_ontvangend = false;

  // Vastgelopen frame (echte storing): na 600ms zonder bytes parser resetten,
  // anders blokkeert een half frame de TX voor altijd
  if(jgc_state != JgcState::WachtStart && millis() - jgc_laatste_byte_ms > 600){
    jgc_state = JgcState::WachtStart;
    jgc_is_ontvangend = false;
    jgc_frame_aborts++;
  }

  uint32_t nu = millis();
  bool na_delay   = (nu - jgc_laatste_rx_einde_ms >= JGC_SEND_DELAY) &&
                    (nu - jgc_laatste_byte_ms     >= JGC_SEND_DELAY) &&
                    !jgc_is_ontvangend;
  bool timeout    = (nu - jgc_laatste_rx_einde_ms >= JGC_SEND_TIMEOUT);
  bool min_interval_ok = (nu - jgc_laatste_send_ms >= JGC_MIN_SEND_INTERVAL);

  if((na_delay || timeout) && min_interval_ok){
    if(timeout && proto_logging) mqtt_log("JGC timeout: geen frame >2s, stuur TX", "WARNING");
    stuur_stand_telegram_jgc();
    jgc_laatste_send_ms = nu;
    vorige_telegram_ms  = nu;
  }

  // Uitgestelde hex-logging: buiten het RX-pad, max één publish per seconde
  static uint32_t laatste_rx_pub_ms = 0;
  if(proto_logging && !jgc_is_ontvangend && millis() - laatste_rx_pub_ms >= 1000){
    if(jgc_tx_pending_len > 0){
      String dec = " | tx" + String(jgc_tx_pending_nr);
      if(jgc_tx_pending_nr == 2)
        dec += " stand=" + String(jgc_tx_pending_buf[4]) +
               (jgc_tx_pending_buf[3] == 0 ? " uit" : (jgc_tx_pending_buf[3] == 2 ? " koeling" : " verwarming"));
      mqtt_proto("tx", jgc_tx_pending_buf, jgc_tx_pending_len, dec);
      jgc_tx_pending_len = 0;
      laatste_rx_pub_ms = millis();
    } else {
      for(uint8_t id = 0; id < 4; id++){
        if(jgc_rx_pending[id]){
          jgc_rx_pending[id] = false;
          mqtt_proto("rx", jgc_frames[id], jgc_rx_len[id], " | id=" + String(id));
          laatste_rx_pub_ms = millis();
          break;  // max één publish per doorgang
        }
      }
    }
  }

  // Fouten-samenvatting: elke 30s één regel (altijd met proto_log aan,
  // anders alleen bij nieuwe fouten)
  static uint32_t laatste_fout_pub_ms = 0;
  static uint16_t vorige_crc_fouten = 0, vorige_aborts = 0;
  static uint32_t vorige_frames_ok = 0;
  if(millis() - laatste_fout_pub_ms >= 30000){
    laatste_fout_pub_ms = millis();
    if(proto_logging ||
       proto_crc_fouten != vorige_crc_fouten || jgc_frame_aborts != vorige_aborts){
      mqtt_log("JGC (30s): CRC +" + String((uint16_t)(proto_crc_fouten - vorige_crc_fouten)) +
               " abort +" + String((uint16_t)(jgc_frame_aborts - vorige_aborts)) +
               " ok +" + String(jgc_frames_ok - vorige_frames_ok) +
               " (totaal " + String(proto_crc_fouten) + "/" + String(jgc_frame_aborts) +
               "/" + String(jgc_frames_ok) + ")", "WARNING");
      vorige_crc_fouten = proto_crc_fouten;
      vorige_aborts = jgc_frame_aborts;
      vorige_frames_ok = jgc_frames_ok;
    }
  }
}

// JGC TX — roteert 4 telegrammen, data2 bevat stand + CRC-CCITT
static uint8_t jgc_tx0[] = { 0x19, 0x0, 0x8, 0x0, 0x0, 0x0, 0xd9, 0xb5 };
static uint8_t jgc_tx1[] = { 0x19, 0x1, 0x0c, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xaa, 0x35 };
static uint8_t jgc_tx2[] = { 0x19, 0x2, 0x8, 0x1, 0x1, 0x0, 0x99, 0x37 };
static uint8_t jgc_tx3[] = { 0x19, 0x3, 0x8, 0xb2, 0x2, 0x0, 0xc1, 0x9a };
static uint8_t jgc_telegram_count = 0;

// Geen mqtt_proto direct na write(): het antwoord van de pomp komt binnen
// ~100ms en een blokkerende publish verstoort dan de RX. TX-frame wordt
// gebufferd en uitgesteld gepubliceerd in lees_warmtepomp_data_jgc().
static void jgc_stash_tx(uint8_t nr, uint8_t* buf, uint8_t len){
  if(!proto_logging) return;
  jgc_tx_pending_nr  = nr;
  jgc_tx_pending_len = len;
  for(uint8_t i = 0; i < len && i < sizeof(jgc_tx_pending_buf); i++) jgc_tx_pending_buf[i] = buf[i];
}

static void stuur_stand_telegram_jgc(){
  switch(jgc_telegram_count){
    case 0:
      chofuSerial.write(jgc_tx0, sizeof(jgc_tx0));
      jgc_stash_tx(0, jgc_tx0, sizeof(jgc_tx0));
      break;
    case 1:
      chofuSerial.write(jgc_tx1, sizeof(jgc_tx1));
      jgc_stash_tx(1, jgc_tx1, sizeof(jgc_tx1));
      break;
    case 2: {
      uint8_t speed = ctrl.stand;
      // 0x00=uit, 0x01=verwarmen, 0x02=koelen (bevestigd door JGC-auteur)
      jgc_tx2[3] = (speed == 0) ? 0x00 : (koeling_modus ? 0x02 : 0x01);
      jgc_tx2[4] = speed;
      uint16_t crc = jgc_bereken_crc(jgc_tx2, sizeof(jgc_tx2) - 2);
      jgc_tx2[6] = (crc >> 8) & 0xFF;
      jgc_tx2[7] = crc & 0xFF;
      chofuSerial.write(jgc_tx2, sizeof(jgc_tx2));
      jgc_stash_tx(2, jgc_tx2, sizeof(jgc_tx2));
      break;
    }
    case 3:
      chofuSerial.write(jgc_tx3, sizeof(jgc_tx3));
      jgc_stash_tx(3, jgc_tx3, sizeof(jgc_tx3));
      break;
  }
  jgc_telegram_count = (jgc_telegram_count + 1) % 4;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIEKE INTERFACE — altijd JGC
// ═══════════════════════════════════════════════════════════════

void stuur_stand_telegram(){
  stuur_stand_telegram_jgc();
}

void lees_warmtepomp_data(){
  lees_warmtepomp_data_jgc();
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
