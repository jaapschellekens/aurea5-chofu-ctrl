#ifndef CHOFU_OT_H
#define CHOFU_OT_H
#include "globals.h"

// ═══════════════════════════════════════════════════════════════
//  OPENTHERM-LINK (inter-MCU verbinding met IC1 in de controlbox)
//  Zie docs/OPENTHERM_LINK_PLAN.md
//
//  FASE 1: ALLEEN LUISTEREN (RX-only). Er wordt nog niets naar IC1
//  gestuurd — de TX-draad hoort fysiek niet aangesloten te zijn.
//
//  Protocol: 9600 baud 8N1, frames van 12 bytes,
//  checksum = som bytes 0..10 mod 256 in byte 11.
//  Byte 0 van IC1→ons = gewenst aanvoersetpoint van Anna (°C).
//
//  Activeren: in config.h
//    #define OT_RX_PIN 8   // via >=1k serie vanaf de IC1-TX pad!
//    #define OT_TX_PIN 9   // gereserveerd voor fase 2, NIET aansluiten
// ═══════════════════════════════════════════════════════════════

// In setup() aanroepen (defines uit config.h zijn alleen in de .ino zichtbaar):
//   #if defined(OT_RX_PIN)
//     ot_config(OT_RX_PIN, OT_TX_PIN);
//   #endif
void        ot_config(uint8_t rx_pin, uint8_t tx_pin);

void        ot_poll();          // elke loop()-doorgang; niet-blokkerend
bool        ot_actief();        // geconfigureerd via ot_config()
bool        ot_geldig();        // >=1 geldig frame en < 30s oud
float       ot_sp();            // laatste setpoint (byte 0), NAN indien ongeldig
bool        ot_mag_zenden();    // veiligheids-gate fase 2: >=3 geldige frames
const char* ot_status_str();    // "uit" / "init" / "ok" / "fout"
uint32_t    ot_frames_ok();     // tellers voor de 30s-samenvatting
uint32_t    ot_frames_fout();
const uint8_t* ot_laatste_frame(); // 12 bytes, voor hex-logging

#endif // CHOFU_OT_H
