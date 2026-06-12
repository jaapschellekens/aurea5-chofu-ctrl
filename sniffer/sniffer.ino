/*
 * CHOFU JGC POLL-SNIFFER - Arduino UNO R4 WiFi (Serial1: D0=RX, D1=TX)
 * --------------------------------------------------------------------
 * Stuurt de 4 JGC-polltelegrammen (roterend, stand=0 = pomp uit) en
 * dumpt ALLES wat terugkomt als hex op de USB-serial. Geen WiFi, geen
 * MQTT — daarmee testen we of de RX-corruptie door de firmware/WiFi
 * komt (sniffer = schoon) of elektrisch is (sniffer = ook rommel).
 *
 * BEKABELING: zoals de hoofdfirmware (pad "RX"→D0, pad "TX"→D1 via 1kΩ).
 * Serial Monitor op 115200.
 */

static uint8_t tx0[] = { 0x19, 0x0, 0x8, 0x0, 0x0, 0x0, 0xd9, 0xb5 };
static uint8_t tx1[] = { 0x19, 0x1, 0x0c, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xaa, 0x35 };
static uint8_t tx2[] = { 0x19, 0x2, 0x8, 0x0, 0x0, 0x0, 0x0, 0x0 };  // stand=0, CRC in setup
static uint8_t tx3[] = { 0x19, 0x3, 0x8, 0xb2, 0x2, 0x0, 0xc1, 0x9a };

static uint16_t crc_ccitt(uint8_t *d, uint8_t len){
  uint16_t crc = 0xFFFF;
  for(uint8_t i = 0; i < len; i++){
    crc ^= (uint16_t)d[i] << 8;
    for(uint8_t j = 0; j < 8; j++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

uint8_t  tx_count = 0;
uint32_t laatste_rx_ms = 0, laatste_tx_ms = 0, laatste_byte_ms = 0;
uint16_t lijn = 0;

void setup(){
  Serial.begin(115200);
  delay(1500);
  Serial1.begin(666);
  uint16_t crc = crc_ccitt(tx2, sizeof(tx2) - 2);
  tx2[6] = (crc >> 8) & 0xFF;
  tx2[7] = crc & 0xFF;
  Serial.println("JGC POLL-SNIFFER (666 baud, stand=0)");
}

void stuur_poll(){
  uint8_t *t; uint8_t n;
  switch(tx_count){
    case 0: t = tx0; n = sizeof(tx0); break;
    case 1: t = tx1; n = sizeof(tx1); break;
    case 2: t = tx2; n = sizeof(tx2); break;
    default: t = tx3; n = sizeof(tx3); break;
  }
  Serial1.write(t, n);
  Serial.print("\n>>> TX");
  Serial.println(tx_count);
  tx_count = (tx_count + 1) % 4;
  lijn = 0;
}

void loop(){
  while(Serial1.available()){
    uint8_t b = Serial1.read();
    uint32_t nu = millis();
    uint32_t gap = nu - laatste_byte_ms;
    if(gap > 40 && laatste_byte_ms != 0){
      Serial.println();
      Serial.print("[+"); Serial.print(gap); Serial.print("ms] ");
      lijn = 0;
    }
    laatste_byte_ms = nu;
    laatste_rx_ms = nu;
    if(b < 0x10) Serial.print('0');
    Serial.print(b, HEX);
    Serial.print(' ');
    if(++lijn >= 32){ Serial.println(); lijn = 0; }
  }

  // Zenden: 99ms na laatste ontvangen byte, of elke 2s als er niets komt;
  // minimaal 300ms tussen polls (zoals JGC origineel)
  uint32_t nu = millis();
  bool na_delay = (nu - laatste_rx_ms >= 99) && !Serial1.available();
  bool timeout  = (nu - laatste_rx_ms >= 2000);
  if((na_delay || timeout) && (nu - laatste_tx_ms >= 300)){
    stuur_poll();
    laatste_tx_ms = nu;
    if(timeout) laatste_rx_ms = nu;  // voorkom poll-storm bij stille lijn
  }
}
