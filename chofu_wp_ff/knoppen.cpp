#include "knoppen.h"
#include "mqtt.h"   // voor mqtt_log() en data_sturen_gevraagd

#define DEBOUNCE_MS   50   // dender-onderdrukking
#define HERHAAL_MS   200   // auto-repeat interval bij ingehouden knop

// ─── Interne helper ────────────────────────────────────────────────────────

static void pas_stand_aan(int delta){
  modus = Modus::HANDMATIG;
  int nieuw = constrain((int)handmatig_stand + delta, 0, 12);
  handmatig_stand = (uint8_t)nieuw;
  data_sturen_gevraagd = true;
  vorige_lcd_ms = 0;    // LCD direct verversen zodat nieuwe stand zichtbaar is
  mqtt_log("Knop: stand=" + String(handmatig_stand), "INFO");
}

// ─── Publieke functies ─────────────────────────────────────────────────────

void knoppen_init(){
  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
}

void check_knoppen(){
  uint32_t nu = millis();

  bool up_raw   = (digitalRead(BTN_UP)   == LOW);
  bool down_raw = (digitalRead(BTN_DOWN) == LOW);

  // Toestandsvariabelen per knop
  static bool     up_prev      = false;
  static uint32_t up_press_ms  = 0;
  static uint32_t up_herhaal_ms = 0;
  static bool     up_active    = false;

  static bool     down_prev      = false;
  static uint32_t down_press_ms  = 0;
  static uint32_t down_herhaal_ms = 0;
  static bool     down_active    = false;

  // ─── UP ────────────────────────────────────────────────────────────────
  if(up_raw && !up_prev)                    up_press_ms = nu;   // leading edge
  if(up_raw && !up_active && nu - up_press_ms >= DEBOUNCE_MS){
    up_active    = true;
    up_herhaal_ms = nu;
    pas_stand_aan(+1);                                           // eerste actie
  }
  if(up_raw && up_active && nu - up_herhaal_ms >= HERHAAL_MS){
    up_herhaal_ms = nu;
    pas_stand_aan(+1);                                           // auto-repeat
  }
  if(!up_raw) up_active = false;
  up_prev = up_raw;

  // ─── DOWN ──────────────────────────────────────────────────────────────
  if(down_raw && !down_prev)                    down_press_ms = nu;
  if(down_raw && !down_active && nu - down_press_ms >= DEBOUNCE_MS){
    down_active    = true;
    down_herhaal_ms = nu;
    pas_stand_aan(-1);
  }
  if(down_raw && down_active && nu - down_herhaal_ms >= HERHAAL_MS){
    down_herhaal_ms = nu;
    pas_stand_aan(-1);
  }
  if(!down_raw) down_active = false;
  down_prev = down_raw;
}
