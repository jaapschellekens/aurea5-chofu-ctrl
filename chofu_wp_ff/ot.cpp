#include "ot.h"
#include "mqtt.h"

// ═══════════════════════════════════════════════════════════════
//  OPENTHERM-LINK — FASE 1: RX-ONLY
//
//  UNO R4: officiële SoftwareSerial uit de renesas-core (GPT-timers
//  + DMA, geen klassiek bit-bangen). EXPERIMENTEEL — afbreekcriteria
//  (zie plan): JGC-statistiek gelijk, MQTT stabiel, geen reboots.
//  ESP32: gewone hardware-UART (UART1) op dezelfde API.
//
//  KRITIEK: niets in dit bestand mag blokkeren (JGC-pomptiming).
// ═══════════════════════════════════════════════════════════════

#if defined(ARDUINO_UNOR4_WIFI)
  #include <SoftwareSerial.h>
  static SoftwareSerial* ot_serial = nullptr;
#else
  static HardwareSerial  ot_serial_hw(1);   // ESP32 UART1
  static HardwareSerial* ot_serial = nullptr;
#endif

static const uint8_t  OT_FRAME_LEN   = 12;
static const uint32_t OT_GELDIG_MS   = 30000;  // IC1 zendt normaal elke ~1-4s
static const uint8_t  OT_TX_GATE_MIN = 3;      // geldige frames vereist vóór ooit zenden

static bool     cfg_ok = false;
static uint8_t  cfg_rx = 0, cfg_tx = 0;
static uint8_t  frame[OT_FRAME_LEN];
static uint8_t  frame_idx = 0;
static uint8_t  laatste_frame[OT_FRAME_LEN] = {0};
static float    ot_sp_val = NAN;
static uint32_t laatste_ok_ms = 0;
static uint32_t frames_ok = 0, frames_fout = 0;
static bool     eerste_frame_gemeld = false;

void ot_config(uint8_t rx_pin, uint8_t tx_pin){
  cfg_rx = rx_pin; cfg_tx = tx_pin;
#if defined(ARDUINO_UNOR4_WIFI)
  ot_serial = new SoftwareSerial(rx_pin, tx_pin);
  ot_serial->begin(9600);
#else
  ot_serial = &ot_serial_hw;
  ot_serial->begin(9600, SERIAL_8N1, rx_pin, tx_pin);
#endif
  cfg_ok = true;
}

bool ot_actief(){ return cfg_ok; }
bool ot_geldig(){ return frames_ok > 0 && (millis() - laatste_ok_ms) < OT_GELDIG_MS; }
float ot_sp(){ return ot_geldig() ? ot_sp_val : NAN; }
bool ot_mag_zenden(){ return ot_geldig() && frames_ok >= OT_TX_GATE_MIN; }
uint32_t ot_frames_ok(){ return frames_ok; }
uint32_t ot_frames_fout(){ return frames_fout; }
const uint8_t* ot_laatste_frame(){ return laatste_frame; }

const char* ot_status_str(){
  if(!cfg_ok) return "uit";
  if(ot_geldig()) return "ok";
  return (frames_ok == 0 && frames_fout == 0) ? "init" : "fout";
}

// Checksum: som bytes 0..10 mod 256 (identiek aan jgc.ino calculateMCUChecksum)
static uint8_t ot_checksum(const uint8_t* d){
  uint16_t sum = 0;
  for(uint8_t i = 0; i < OT_FRAME_LEN - 1; i++) sum += d[i];
  return (uint8_t)(sum & 0xFF);
}

void ot_poll(){
  if(!cfg_ok) return;

  while(ot_serial->available()){
    frame[frame_idx++] = (uint8_t)ot_serial->read();
    if(frame_idx < OT_FRAME_LEN) continue;

    if(ot_checksum(frame) == frame[OT_FRAME_LEN - 1]){
      // Geldig frame
      memcpy(laatste_frame, frame, OT_FRAME_LEN);
      frames_ok++;
      laatste_ok_ms = millis();
      // Byte 0 = gewenst aanvoersetpoint van Anna. Clamp zoals jgc.ino:
      // 0xF0 e.d. zijn commandowaarden, alleen 0..90 als temperatuur nemen.
      uint8_t sp = frame[0];
      ot_sp_val = (sp <= 90) ? (float)sp : NAN;
      if(!eerste_frame_gemeld){
        eerste_frame_gemeld = true;
        mqtt_log("OT-link: eerste geldige frame ontvangen (SP byte0=" + String(sp) + ")", "INFO");
      }
      frame_idx = 0;
    } else {
      // Checksum fout: één byte opschuiven (sliding window) tot we
      // weer synchroon lopen — géén mqtt_log hier (RX-pad!)
      frames_fout++;
      memmove(frame, frame + 1, OT_FRAME_LEN - 1);
      frame_idx = OT_FRAME_LEN - 1;
    }
  }

  // FASE 2 (nog niet actief): zenden naar IC1. Harde gate: nooit zenden
  // zonder ot_mag_zenden() — beschermt IC1 tegen verwisselde bedrading.
}
