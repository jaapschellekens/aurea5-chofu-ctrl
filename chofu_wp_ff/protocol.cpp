#include "protocol.h"
#include "mqtt.h"   // voor stuur_alert()

uint8_t bereken_checksum(uint8_t *buf, uint8_t len){
  uint16_t sum = 0;
  for(uint8_t i=0; i<len; i++) sum += buf[i];
  return (sum & 0xFF);
}

void stuur_stand_telegram(){
  uint8_t telegram[25] = {0};
  telegram[0] = 0x19;
  telegram[1] = ctrl.stand;
  telegram[2] = 0x00;
  telegram[3] = (ctrl.stand == 0) ? 0 : (koeling_modus ? 2 : 1);
  telegram[23] = bereken_checksum(telegram, 23);
  telegram[24] = 0x00;
  chofuSerial.write(telegram, 25);
  Serial.print("TX: Stand ");Serial.print(ctrl.stand);Serial.println(" naar WP");
}

void verwerk_telegram_0x91(){
  if(telegram_buffer[0] != 0x91) return;
  uint8_t calc_cs = bereken_checksum(telegram_buffer, 23);
  if(calc_cs != telegram_buffer[23]){ Serial.println("RX: checksum fout"); return; }

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

  static uint8_t debug_count = 0;
  if(debug_count++ % 10 == 0){
    Serial.print("RX WP: A:");Serial.print(t_supply,1);
    Serial.print(" R:");Serial.print(t_return,1);
    Serial.print(" B:");Serial.print(t_outside,1);
    Serial.print(" Hz:");Serial.print(comp_hz);
    Serial.print(" P:");Serial.print(pomp_snelheid_wp);Serial.println("%");
  }
}

void lees_warmtepomp_data(){
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
    stuur_stand_telegram();
    vorige_telegram_ms = millis();
  }
}

void pas_sim_toe(){
  if(!isnan(sim_t_supply))        { t_supply        = sim_t_supply;        prev_t_supply  = sim_t_supply; }
  if(!isnan(sim_t_return))        { t_return        = sim_t_return;        prev_t_return  = sim_t_return; }
  if(!isnan(sim_t_outside))       { t_outside       = sim_t_outside;       prev_t_outside = sim_t_outside; }
  if(!isnan(sim_t_water_gewenst)) { t_water_gewenst = sim_t_water_gewenst; }
  if(!isnan(sim_t_kamer))         { t_kamer         = sim_t_kamer; }
  if(!isnan(sim_t_kamer_gewenst)) { t_kamer_gewenst = sim_t_kamer_gewenst; }
  if(sim_actief()) delta_t = t_supply - t_return;
}
