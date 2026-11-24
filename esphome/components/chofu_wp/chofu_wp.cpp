// Kopie van components/chofu_wp uit Aapjedotinfo/atlantic-aurea-esphome @ 119e800,
// met eigen toevoegingen (aurea5-chofu-ctrl, 2026-11-23):
//  - frames zonder 00-eindbyte worden niet meer weggegooid (CRC zonder laatste byte),
//  - ID2-gat: >15 s stand vasthouden, >120 s temperaturen NaN, >15 min stand 0 (geen blinde regeling),
//  - elke 30 s een RX-diagnose als er frames afgekeurd worden of ID2 uitblijft.
#include "chofu_wp.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace chofu_wp {

static const char *const TAG = "chofu_wp";

// ═══════════════════════════════════════════════════════════════
//  De 4 roterende zend-telegrammen (controlbox -> warmtepomp)
//  data0/1/3 zijn vast; data2 wordt per keer opgebouwd (on/off + stand + CRC).
//  Bytes exact uit de werkende sketches van WackoH (en JGC), zie chofu_wp.h.
// ═══════════════════════════════════════════════════════════════
static const uint8_t DATA0[] = {0x19, 0x00, 0x08, 0x00, 0x00, 0x00, 0xD9, 0xB5};
static const uint8_t DATA1[] = {0x19, 0x01, 0x0C, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0xAA, 0x35};
static const uint8_t DATA3[] = {0x19, 0x03, 0x08, 0xB2, 0x02, 0x00, 0xC1, 0x9A};

// Geldige berichtlengtes vanuit de WP (msgLength = lengtebyte + 1), uit de sketches van WackoH.
static const uint8_t VALID_LENGTHS[] = {13, 14, 19, 21};

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

void ChofuWP::setup() {
  ESP_LOGI(TAG, "Chofu 0x19-protocol gestart (666 baud, 4 roterende telegrammen)");
}

void ChofuWP::dump_config() {
  ESP_LOGCONFIG(TAG, "Chofu WP (666 baud):");
  ESP_LOGCONFIG(TAG, "  Setpoint aanvoer: %.1f°C (bereik %.0f-%.0f)", setpoint_, setpoint_min_,
                setpoint_max_);
  ESP_LOGCONFIG(TAG, "  Bedrijfsmodus: %s", cooling_ ? "koelen" : "verwarmen");
  ESP_LOGCONFIG(TAG, "  Regeling: delta-T-band %.1f-%.1f°C, max stand %u, stap elke %us", dt_low_,
                dt_high_, max_stand_, (unsigned) (step_interval_ms_ / 1000));
  ESP_LOGCONFIG(TAG, "  Stooklijn: %.1f°C bij -10, %.1f°C bij +15, stookgrens %.1f°C", curve_cold_,
                curve_warm_, heat_limit_);
  this->check_uart_settings(666);
}

// ═══════════════════════════════════════════════════════════════
//  CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, geen reflectie)
//  CRC over heel bericht incl. de 2 CRC-bytes == 0 bij foutvrije ontvangst.
// ═══════════════════════════════════════════════════════════════
uint16_t ChofuWP::crc_ccitt_(const uint8_t *data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t) data[i] << 8;
    for (uint8_t j = 0; j < 8; j++)
      crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
  }
  return crc;
}

bool ChofuWP::valid_msg_length_(uint8_t len) {
  for (uint8_t v : VALID_LENGTHS)
    if (len == v)
      return true;
  return false;
}

// ═══════════════════════════════════════════════════════════════
//  loop(): RX-bytes verwerken + (op de juiste momenten) een telegram zenden
// ═══════════════════════════════════════════════════════════════
void ChofuWP::loop() {
  this->handle_rx_();
  this->maybe_send_();
}

// ═══════════════════════════════════════════════════════════════
//  RX - 0x91 berichten van de warmtepomp, per ID (state machine uit de sketches van WackoH)
// ═══════════════════════════════════════════════════════════════
void ChofuWP::handle_rx_() {
  while (this->available()) {
    uint8_t b;
    if (!this->read_byte(&b))
      break;
    is_receiving_ = true;
    last_byte_ms_ = millis();  // Voor de lijn-stiltebewaking in maybe_send_()

    switch (rx_state_) {
      case WAIT_START:
        if (b == 0x91) {
          rx_state_ = READ_HEADER;
          rx_hdr_idx_ = 0;
          frame_[0] = b;
          frame_len_ = 1;
        }
        break;

      case READ_HEADER:
        if (frame_len_ < sizeof(frame_))
          frame_[frame_len_++] = b;
        if (rx_hdr_idx_ == 0) {
          rx_id_ = b;
          if (rx_id_ > 3) {  // ID moet 0-3 zijn
            this->abort_frame_("ongeldig ID");
            break;
          }
        } else if (rx_hdr_idx_ == 1) {
          uint8_t msg_length = b + 1;
          if (!valid_msg_length_(msg_length)) {
            this->abort_frame_("ongeldige lengte");
            break;
          }
          rx_datalen_ = msg_length - 3;
          rx_idx_ = 0;
          rx_state_ = READ_PAYLOAD;
        }
        rx_hdr_idx_++;
        break;

      case READ_PAYLOAD:
        if (frame_len_ < sizeof(frame_))
          frame_[frame_len_++] = b;
        rx_idx_++;
        if (rx_idx_ >= rx_datalen_) {
          // Frame compleet: 0x91 + ID + len + <datalen> payload (laatste 2 = CRC).
          // GEEN aparte end-byte - de volgende byte is al het volgende telegram.
          rx_state_ = WAIT_START;  // vóór finish_frame_: die kan een geleende 0x91 als nieuw begin zetten
          this->finish_frame_();
        }
        break;
    }
  }
}

// Frame compleet: CRC valideren (CRC over heel frame == 0) + waarden extraheren.
void ChofuWP::finish_frame_() {
  is_receiving_ = false;
  prev_msg_in_ended_ms_ = millis();  // ALTIJD - zo volgt de TX-timing de WP-cadans

  const uint16_t crc = crc_ccitt_(frame_, frame_len_);

  if (raw_debug_) {
    char hex[sizeof(frame_) * 3 + 1];
    for (size_t i = 0; i < frame_len_; i++)
      sprintf(hex + i * 3, "%02X ", frame_[i]);
    hex[frame_len_ * 3] = '\0';
    ESP_LOGD(TAG, "FRAME ID%u [%u] %s crc=%04X %s", rx_id_, (unsigned) frame_len_, hex, crc,
             crc == 0 ? "OK" : "FOUT");
  }

  bool borrowed_start = false;
  if (crc != 0) {
    // Ontbreekt de 00-eindbyte, dan heeft de parser de eerste byte van het
    // VOLGENDE frame als laatste byte meegenomen. Klopt de CRC zonder die
    // byte, dan is het frame goed; is die byte 0x91, dan begint daar het
    // volgende frame (anders zou ook dat frame verloren gaan).
    if (frame_len_ > 3 && crc_ccitt_(frame_, frame_len_ - 1) == 0) {
      borrowed_start = frame_[frame_len_ - 1] == 0x91;
      frame_len_--;
      if (rx_id_ < 4 && no_term_[rx_id_] < 0xFFFF)
        no_term_[rx_id_]++;
    } else {
      if (rx_id_ < 4 && crc_fail_[rx_id_] < 0xFFFF)
        crc_fail_[rx_id_]++;
      for (size_t i = 0; i < frame_len_; i++)
        sprintf(last_bad_ + i * 3, "%02X ", frame_[i]);
      last_bad_[frame_len_ * 3] = '\0';
      return;  // Corrupt frame negeren
    }
  }

  last_valid_rx_ms_ = millis();
  if (rx_id_ < 4)
    last_id_ms_[rx_id_] = last_valid_rx_ms_;

  // Temperaturen: signed 16-bit LITTLE-endian (LSB eerst), /10.
  switch (rx_id_) {
    case 1:
      // ID1 idx4 was aangemerkt als "defrost", maar metingen laten zien dat
      // die byte gewoon 0x01 wordt zodra de unit draait (en 0x00 als hij stil
      // staat) - het is een bedrijfsstatus, geen defrost. Tot we de echte
      // defrost-byte kennen niet meer als defrost publiceren.
      // Naar de byte-map van WackoH (Tweakers, GitHub issue #1) is byte 4 wel degelijk "ontdooien actief" (0/1) en byte 3 de
      // bedrijfsstatus (0x50 verwarmen, 0x40 stop voor of tijdens ontdooien, 0x00 uit); byte 10 is het ventilator-setpoint (x10 rpm).
      run_status_ = frame_[3];
      defrost_flag_ = frame_[4];
      fan_set_rpm_ = frame_[10] * 10.0f;
      break;
    case 2:
      t_supply_ = (int16_t) ((frame_[4] << 8) | frame_[3]) / 10.0f;
      t_return_ = (int16_t) ((frame_[6] << 8) | frame_[5]) / 10.0f;
      t_outside_ = (int16_t) ((frame_[8] << 8) | frame_[7]) / 10.0f;
      if (frame_[9] == 0) {  // gegevenspagina (01 = constante pagina), naar WackoH: T = raw/2 - 41
        t_defrost_ = frame_[11] / 2.0f - 41.0f;
        t_suction_ = frame_[12] / 2.0f - 41.0f;
        t_discharge_ = (float) frame_[13];
        t_water_ = frame_[14] / 2.0f - 41.0f;
      }
      break;
    case 3:
      wp_speed_ = frame_[9];
      // 2-11-2026 tegen een Shelly nagemeten (48 uur): byte 10 volgt het vermogen met ~24 W per telling
      // (+ ~100 W pomp/elektronica): byte 10 is de stroom in 0,1 A (WackoH). Byte 9 loopt er recht mee
      // (ongeveer 1,16 x byte 10) maar is GEEN stroom in 0,2 A zoals hier eerder stond.
      wp_power_w_ = 25.6f * frame_[10];
      // Naar de byte-map van WackoH: byte 3 = compressorfrequentie (x 2/3 Hz), byte 4 = ventilator (x10 rpm),
      // byte 6 = expansieventiel (x2 pulsen), byte 8 = statusbits (bit 0 ontdooien), byte 9 = stroom (/5 A).
      freq_ = frame_[3] * 2.0f / 3.0f;
      fan_rpm_ = frame_[4] * 10.0f;
      eev_ = frame_[6] * 2.0f;
      status8_ = frame_[8];
      current_ = frame_[10] / 10.0f;
      pump_byte_ = frame_[13];
      break;
    default:
      break;
  }

  if (borrowed_start) {
    // De geleende 0x91 is het begin van het volgende frame.
    frame_[0] = 0x91;
    frame_len_ = 1;
    rx_state_ = READ_HEADER;
    rx_hdr_idx_ = 0;
    is_receiving_ = true;
  }
}

// Frame afgekeurd in de header (ongeldig ID/lengte). Belangrijk: ook hier de
// timing bijwerken, anders blijft is_receiving_ hangen en valt maybe_send_()
// terug op de blinde timeout - precies wat botsingen veroorzaakte.
void ChofuWP::abort_frame_(const char *reason) {
  if (aborts_ < 0xFFFF)
    aborts_++;
  for (size_t i = 0; i < frame_len_; i++)
    sprintf(last_bad_ + i * 3, "%02X ", frame_[i]);
  last_bad_[frame_len_ * 3] = '\0';
  if (raw_debug_)
    ESP_LOGD(TAG, "RX afgebroken (%s) - resync", reason);
  is_receiving_ = false;
  prev_msg_in_ended_ms_ = millis();
  rx_state_ = WAIT_START;
}

// ═══════════════════════════════════════════════════════════════
//  TX - stuur het volgende telegram als de master/slave-timing dat toelaat
// ═══════════════════════════════════════════════════════════════
void ChofuWP::maybe_send_() {
  const uint32_t now = millis();

  // HARDE botsingsbescherming: nooit zenden zolang er nog bytes binnendruppelen.
  // Vertrouwt niet op de parser-state (die kan ontsporen op een corrupt frame),
  // maar puur op de lijn zelf. Voorkomt dat ons telegram midden in een frame
  // van de warmtepomp belandt.
  if (now - last_byte_ms_ < LINE_IDLE_MS)
    return;

  const bool after_rx = (now - prev_msg_in_ended_ms_ >= SEND_DELAY_MS) && !is_receiving_;
  const bool timed_out = (now - prev_msg_in_ended_ms_ >= SEND_TIMEOUT_MS);
  const bool spaced = (now - prev_msg_sent_ms_ >= MIN_SEND_INTERVAL_MS);
  if (!((after_rx || timed_out) && spaced))
    return;

  switch (telegram_count_) {
    case 0:
      this->write_array(DATA0, sizeof(DATA0));
      break;
    case 1:
      this->write_array(DATA1, sizeof(DATA1));
      break;
    case 2: {
      uint8_t data2[8];
      this->build_data2_(data2);
      this->write_array(data2, sizeof(data2));
      break;
    }
    case 3:
      this->write_array(DATA3, sizeof(DATA3));
      break;
  }

  telegram_count_ = (telegram_count_ + 1) % 4;
  prev_msg_sent_ms_ = now;
}

// data2: 19 02 08 <modus> <stand> 00 <crc_hi> <crc_lo>
// modus: 0x00 = uit, 0x01 = verwarmen, 0x02 = koelen.
// LET OP: 0x02 komt uit één forumpost en is in de sketches niet gebruikt - het is
// dus experimenteel. De WP negeert het mogelijk gewoon.
void ChofuWP::build_data2_(uint8_t *out) {
  out[0] = 0x19;
  out[1] = 0x02;
  out[2] = 0x08;
  if (stand_ == 0) {
    out[3] = 0x00;  // uit
    out[4] = 0x00;
  } else {
    out[3] = cooling_ ? 0x02 : 0x01;  // koelen of verwarmen
    out[4] = stand_;
  }
  out[5] = 0x00;
  uint16_t crc = crc_ccitt_(out, 6);
  out[6] = (crc >> 8) & 0xFF;  // high byte
  out[7] = crc & 0xFF;         // low byte
}

// ═══════════════════════════════════════════════════════════════
//  Regeling - elke 5s (update_interval).
//  Eerst het doel (uit de modus), dan aanvoer -> stand.
// ═══════════════════════════════════════════════════════════════
void ChofuWP::update() {
  this->run_control_();
  this->report_diag_(millis());
}

// Elke 30 s een waarschuwing, alleen als er iets mis is: afgekeurde frames,
// frames zonder eindbyte, of te lang geen ID2. Werkt ook met "Debug frames" uit.
void ChofuWP::report_diag_(uint32_t now) {
  if (now - diag_last_ms_ < DIAG_EVERY_MS)
    return;
  diag_last_ms_ = now;
  uint16_t crc = 0, noterm = 0;
  for (uint8_t i = 0; i < 4; i++) {
    crc += crc_fail_[i];
    noterm += no_term_[i];
  }
  const int id2 = get_id2_age_s();
  const bool nieuw = crc != diag_crc_prev_ || noterm != diag_noterm_prev_ || aborts_ != diag_abort_prev_;
  if (!nieuw && !(id2 > 30))
    return;
  ESP_LOGW(TAG,
           "RX-diagnose: ID2 %d s geleden | CRC-fout per ID %u/%u/%u/%u | zonder eindbyte %u/%u/%u/%u | "
           "afgebroken %u | laatst afgekeurd: %s",
           id2, crc_fail_[0], crc_fail_[1], crc_fail_[2], crc_fail_[3], no_term_[0], no_term_[1], no_term_[2],
           no_term_[3], aborts_, last_bad_[0] ? last_bad_ : "-");
  diag_crc_prev_ = crc;
  diag_noterm_prev_ = noterm;
  diag_abort_prev_ = aborts_;
}

void ChofuWP::set_reason_(const char *r) {
  if (strcmp(r, reason_) == 0)
    return;
  reason_ = r;
  ESP_LOGI(TAG, "Regeling: %s", r);
}

uint8_t ChofuWP::avg_samples_() const {
  // Eén monster per 10 s. Venster 0 = geen middeling = het laatste monster.
  if (avg_s_ <= 0.0f)
    return 1;
  int n = (int) lroundf(avg_s_ / 10.0f);
  return (uint8_t) clamp(n, 1, 20);
}

// Gemiddelde van n monsters, te beginnen "from" monsters terug (0 = nieuwste).
// Neemt minder monsters als de historie nog korter is; NaN-monsters tellen niet mee.
float ChofuWP::avg_hist_(const float *h, uint8_t from, uint8_t n) const {
  if (hist_cnt_ <= from)
    return NAN;
  if ((int) from + n > hist_cnt_)
    n = (uint8_t) (hist_cnt_ - from);
  float sum = 0.0f;
  uint8_t cnt = 0;
  for (uint8_t k = 0; k < n; k++) {
    const uint8_t idx = (uint8_t) ((hist_idx_ + HIST_N - 1 - from - k) % HIST_N);
    if (!std::isnan(h[idx])) {
      sum += h[idx];
      cnt++;
    }
  }
  return cnt ? sum / cnt : NAN;
}

float ChofuWP::get_rise_() const {
  // Gemiddelde van 5 monsters nu tegen 5 monsters ~280 s eerder, omgerekend
  // naar 300 s. Een enkel monster zou het gezaag van de aanvoer meten.
  if (hist_cnt_ < 33)
    return NAN;
  const float nieuw = avg_hist_(sup_hist_, 0, 5);
  const float oud = avg_hist_(sup_hist_, 28, 5);
  if (std::isnan(nieuw) || std::isnan(oud))
    return NAN;
  return (nieuw - oud) * 30.0f / 28.0f;
}

float ChofuWP::get_rate_() const {
  // Gemiddelde over het venster nu tegen hetzelfde venster 60 s eerder.
  const uint8_t w = avg_samples_();
  if (hist_cnt_ < 6 + w)
    return NAN;
  const float nieuw = avg_hist_(sup_hist_, 0, w);
  const float oud = avg_hist_(sup_hist_, 6, w);
  if (std::isnan(nieuw) || std::isnan(oud))
    return NAN;
  return nieuw - oud;
}

float ChofuWP::get_curve_supply() const {
  if (std::isnan(t_outside_))
    return NAN;
  // Recht tussen (-10, curve_cold_) en (+15, curve_warm_), daarbuiten vast.
  const float b = clamp(t_outside_, -10.0f, 15.0f);
  return curve_cold_ + (curve_warm_ - curve_cold_) * (b + 10.0f) / 25.0f;
}

bool ChofuWP::get_room_fresh() const {
  return room_ms_ != 0 && !std::isnan(room_temp_) && (millis() - room_ms_) < room_timeout_ms_;
}

float ChofuWP::target_supply_(uint32_t now) {
  if (mode_ == MODE_AUTO || cooling_) {
    // Koelen kent geen stooklijn: dan altijd het vaste setpoint.
    set_reason_(mode_ == MODE_AUTO ? "vast setpoint" : "koelen: vast setpoint");
    return setpoint_;
  }

  if (std::isnan(t_outside_)) {
    set_reason_("geen buitentemperatuur - unit uit");
    return NAN;
  }
  // Stookgrens met 1 °C hysterese: de buitenvoeler zit op de unit zelf en
  // beweegt mee met zon en ontdooien.
  if (t_outside_ >= heat_limit_)
    above_heat_limit_ = true;
  else if (t_outside_ < heat_limit_ - 1.0f)
    above_heat_limit_ = false;
  if (above_heat_limit_) {
    set_reason_("boven de stookgrens - geen warmtevraag");
    return NAN;
  }

  const float curve = get_curve_supply();
  if (mode_ == MODE_CURVE) {
    set_reason_("stooklijn");
    return curve;
  }

  // ── Kamertemperatuur ──
  if (!get_room_fresh()) {
    room_i_ = 0.0f;
    room_i_ms_ = 0;
    set_reason_("kamertemperatuur ontbreekt of is verlopen - unit uit");
    return NAN;
  }
  const float fout = room_target_ - room_temp_;  // > 0 = te koud

  if (room_off_warm_) {
    // Minimale schakeltijd: na een wisseling van vraag blijft die minstens zo lang staan, zodat
    // koken of apparaten in huis hem niet steeds aan en uit zetten. 0 = zoals voorheen.
    const bool vast_houden = room_min_hold_ms_ > 0 && room_demand_ms_ != 0 && (now - room_demand_ms_) < room_min_hold_ms_;
    if (!vast_houden) {
      if (room_demand_ && fout < -room_hyst_off_) {
        room_demand_ = false;
        room_demand_ms_ = now;
      } else if (!room_demand_ && fout > room_hyst_on_) {
        room_demand_ = true;
        room_demand_ms_ = now;
      }
    }
  } else {
    room_demand_ = true;
    room_demand_ms_ = 0;
  }

  // Nastelling: eens per minuut, en alleen zolang er vraag is.
  if (room_ki_ <= 0.0f) {
    room_i_ = 0.0f;
  } else if (room_i_ms_ == 0) {
    room_i_ms_ = now;
  } else if (now - room_i_ms_ >= 60000) {
    const float minuten = (now - room_i_ms_) / 60000.0f;
    room_i_ms_ = now;
    const float zonder = curve + room_gain_ * fout + room_i_;
    // Niet verder oplopen als het doel al tegen een grens zit (anti-windup).
    const bool vast = (fout > 0 && zonder >= setpoint_max_) || (fout < 0 && zonder <= HEAT_MIN_SUPPLY);
    if (room_demand_ && !vast)
      room_i_ = clamp(room_i_ + room_ki_ * fout * minuten, ROOM_I_MIN, ROOM_I_MAX);
  }

  if (!room_demand_) {
    set_reason_("kamer warm genoeg - unit uit");
    return NAN;
  }
  set_reason_("kamertemperatuur");
  return clamp(curve + room_gain_ * fout + room_i_, HEAT_MIN_SUPPLY, setpoint_max_);
}

void ChofuWP::run_control_() {
  const uint32_t now = millis();

  // Geen verse temperaturen (zie ID2_*_MS in chofu_wp.h): eerst stand vasthouden,
  // daarna "onbekend" in plaats van een bevroren waarde, en pas na lange tijd uit.
  const uint32_t id2_age = last_id_ms_[2] == 0 ? 0 : now - last_id_ms_[2];
  const bool id2_hold = last_id_ms_[2] != 0 && id2_age > ID2_HOLD_MS;
  const bool id2_stop = id2_hold && id2_age > ID2_STOP_MS;
  if (id2_hold && id2_age > ID2_STALE_MS) {
    t_supply_ = t_return_ = t_outside_ = NAN;
    t_defrost_ = t_suction_ = t_discharge_ = t_water_ = NAN;
  }
  if (id2_hold && !id2_stale_)
    ESP_LOGW(TAG, "Al %u s geen geldig ID2-frame - stand %u vastgehouden", (unsigned) (id2_age / 1000), stand_);
  else if (!id2_hold && id2_stale_)
    ESP_LOGI(TAG, "ID2-frames weer binnen - regeling hervat");
  id2_stale_ = id2_hold;

  // Historie van aanvoer en retour bijhouden, ook als hij stilstaat - maar niet
  // met bevroren waarden tijdens een ID2-gat (dat vertekent stijging en middeling).
  if (!id2_hold && !std::isnan(t_supply_) && (hist_last_ms_ == 0 || now - hist_last_ms_ >= 10000)) {
    sup_hist_[hist_idx_] = t_supply_;
    ret_hist_[hist_idx_] = t_return_;
    hist_idx_ = (uint8_t) ((hist_idx_ + 1) % HIST_N);
    if (hist_cnt_ < HIST_N)
      hist_cnt_++;
    hist_last_ms_ = now;
  }

  // Master-schakelaar uit -> alles uit
  if (!system_on_) {
    stand_ = 0;
    sp_eff_ = NAN;
    set_reason_("systeem uit");
    return;
  }

  // Handmatige modus -> stand rechtstreeks
  if (mode_ == MODE_MANUAL) {
    stand_ = clamp<uint8_t>(manual_stand_, 0, max_stand_);
    sp_eff_ = NAN;
    set_reason_("handmatig");
    return;
  }

  // Fail-safe: geen geldig telegram -> niet blind vermogen blijven vragen
  if (last_valid_rx_ms_ == 0 || (now - last_valid_rx_ms_) > COMM_TIMEOUT_MS) {
    if (stand_ != 0)
      ESP_LOGW(TAG, "Geen geldige communicatie - terug naar stand 0");
    stand_ = 0;
    set_reason_("geen communicatie met de warmtepomp");
    return;
  }

  if (id2_stop) {
    if (stand_ != 0)
      ESP_LOGW(TAG, "Al %u min geen ID2-frame - unit uit", (unsigned) (id2_age / 60000));
    stand_ = 0;
    sp_eff_ = NAN;
    set_reason_("geen temperatuurdata van de warmtepomp (ID2) - unit uit");
    return;
  }
  if (id2_hold) {
    set_reason_("geen temperatuurdata van de warmtepomp (ID2) - stand vastgehouden");
    return;
  }

  if (std::isnan(t_supply_) || std::isnan(t_return_))
    return;  // Nog geen data binnen; huidige stand aanhouden

  // Regeltemperaturen: gemiddeld over het middelingsvenster; zonder venster of
  // zonder historie de ruwe waarde.
  float rs = t_supply_;
  float rr = t_return_;
  if (avg_s_ > 0.0f) {
    const float rs_gem = avg_hist_(sup_hist_, 0, avg_samples_());
    const float rr_gem = avg_hist_(ret_hist_, 0, avg_samples_());
    if (!std::isnan(rs_gem)) rs = rs_gem;
    if (!std::isnan(rr_gem)) rr = rr_gem;
  }

  // ── Het doel ──
  // Alleen richting MEER vraag schuift het geleidelijk; minder vraag gaat direct.
  // Na stilstand begint het schuiven bij de huidige aanvoer, anders ziet een
  // koude start meteen het hele verschil als fout en schakelt hij door naar een
  // stand waar hij later doorheen schiet.
  const float doel = this->target_supply_(now);
  if (std::isnan(doel)) {
    sp_eff_ = NAN;
  } else if (slew_c_per_min_ <= 0.0f) {
    sp_eff_ = doel;
  } else {
    if (std::isnan(sp_eff_))
      sp_eff_ = cooling_ ? fmaxf(doel, rs) : fminf(doel, rs);
    const float max_stap = slew_c_per_min_ * (float) (now - sp_eff_ms_) / 60000.0f;
    if (cooling_)
      sp_eff_ = doel < sp_eff_ ? fmaxf(doel, sp_eff_ - max_stap) : doel;
    else
      sp_eff_ = doel > sp_eff_ ? fminf(doel, sp_eff_ + max_stap) : doel;
    // Nooit beginnen onder de huidige aanvoer (bij koelen: erboven). Na een
    // herstart stond het doel op een oud bewaard setpoint (17-10-2026: 20 °C
    // terwijl de aanvoer 22 was); van daaruit oplopen hield de unit minutenlang
    // stil zonder dat dat ergens tegen beschermde.
    if (cooling_)
      sp_eff_ = fminf(sp_eff_, fmaxf(doel, rs));
    else
      sp_eff_ = fmaxf(sp_eff_, fminf(doel, rs));
  }
  sp_eff_ms_ = now;

  // Koelen keert alles om: dan wil je de aanvoer juist OMLAAG naar het
  // setpoint, en is de retour warmer dan de aanvoer. Door beide grootheden
  // mode-afhankelijk te definiëren blijft de regel-tabel hieronder identiek:
  // err > 0 = meer vermogen nodig, dt = hoeveel de WP daadwerkelijk verzet.
  // Geen vraag (doel NaN) telt als "veel te warm": rustig afbouwen naar 0.
  const float sp = sp_eff_;
  const float dt = cooling_ ? (rr - rs) : (rs - rr);
  const float err = std::isnan(sp) ? -100.0f : (cooling_ ? (rs - sp) : (sp - rs));
  const float band = 0.3f;                       // dode zone rond setpoint

  // Absolute vorstbeveiliging bij koelen (default 5°C, onder de fabrieksgrens
  // van 6.5°C - zit legitiem bedrijf dus nooit in de weg). Op de eigen, ruwe
  // aanvoer: een gemiddelde zou te laat zijn.
  if (cooling_ && t_supply_ < cooling_min_supply_) {
    if (stand_ != 0)
      ESP_LOGW(TAG, "Koelen: aanvoer %.1f°C te laag - stop", t_supply_);
    stand_ = 0;
    return;
  }

  // ── Richting kiezen: +1 meer / -1 minder / 0 houden ──
  // Delta-T tempert de keuze: aanvoer te koud MAAR delta-T al hoog -> HOUDEN.
  // (Retour warmt vanzelf op; extra gas zou overschieten.) Bewust omgekeerd
  // t.o.v. de oude .ino-formule die juist bijstookte bij hoge delta-T.
  int8_t dir = 0;
  if (err > band) {                       // aanvoer te laag -> meer willen...
    dir = (dt >= dt_high_) ? 0 : +1;      // ...tenzij delta-T al te hoog is
  } else if (err < -band) {               // aanvoer te hoog -> minder
    dir = -1;
  } else {                                // rond setpoint
    if (dt > dt_high_)                     // levert te agressief -> gas terug
      dir = -1;
  }

  // ── Vooruitkijken ──
  // De compressor werkt traag door: een stand later is de aanvoer pas echt
  // anders, en bij stand 3-4 stijgt hij 1-2 °C per minuut. Wachten tot hij
  // boven het setpoint zit is dan te laat.
  bool vroeg_remmen = false;
  if (lookahead_min_ > 0.0f && !std::isnan(sp)) {
    const float rate = get_rate_();
    if (!std::isnan(rate)) {
      const float rs_verwacht = rs + rate * lookahead_min_;
      const float err_verwacht = cooling_ ? (rs_verwacht - sp) : (sp - rs_verwacht);
      if (dir > 0 && stand_ > 0 && err_verwacht <= band) {
        dir = 0;                                      // komt er vanzelf: niet nog een stand erbij
      } else if (dir <= 0 && stand_ > 1 && err_verwacht < -BRAKE_OVERSHOOT_K) {
        dir = -1;                                     // schiet ruim door: nu al een stand terug
        vroeg_remmen = true;
      }
    }
  }

  // ── Stoppen (stand 1 -> 0) met warmtevraag: eerst een tijd achter elkaar willen, en alleen als de
  // aanvoer na een stop niet meteen weer te laag zou zijn ──
  // Na een stop zakt de aanvoer binnen ~1 minuut naar de retour. Voorspelde aanvoer over de
  // vooruitkijktijd: retour + (aanvoer - retour) * exp(-t/60 s). Ligt die onder het setpoint min de dode
  // zone, dan zou hij meteen weer starten, en dan stopt hij niet. Geen warmtevraag (sp NaN) en koelen
  // houden het oude gedrag; met "Stopvertraging" op 0 is alles als voorheen.
  const bool wil_stop = !cooling_ && !std::isnan(sp) && stand_ == 1 && dir < 0;
  if (wil_stop && stop_delay_ms_ > 0) {
    if (stop_wish_ms_ == 0)
      stop_wish_ms_ = now;
    const float t_min = lookahead_min_ > 0.0f ? lookahead_min_ : 2.0f;
    const float pred = rr + (rs - rr) * expf(-t_min);   // tau = 60 s: t in minuten is direct de exponent
    const bool wachten = (now - stop_wish_ms_) < stop_delay_ms_;
    const bool terug_nodig = pred < sp - band;
    if (wachten || terug_nodig) {
      if (!stop_hold_) {
        ESP_LOGI(TAG, "AUTO: stoppen uitgesteld (%s): aanvoer %.1f/%.1f, voorspeld na stop %.1f",
                 terug_nodig ? "zou meteen weer nodig zijn" : "eerst stopvertraging", rs, sp, pred);
        stop_hold_ = true;
      }
      dir = 0;
    } else {
      stop_hold_ = false;
    }
  }
  if (!wil_stop) {
    stop_wish_ms_ = 0;
    stop_hold_ = false;
  }

  // ── Nog aan het opwarmen? Dan niet opschakelen ──
  // Stijgt de aanvoer nog flink, dan werkt de vorige stap nog door; wachten
  // voorkomt dat hij doorschakelt terwijl het water gewoon nog opwarmt.
  // Bij koelen telt dalen als "stijgen".
  if (dir > 0 && stand_ > 0 && rise_limit_ > 0.0f) {
    float rise = get_rise_();
    if (cooling_ && !std::isnan(rise))
      rise = -rise;
    if (!std::isnan(rise) && rise > rise_limit_) {
      if (!rise_holding_)
        ESP_LOGI(TAG, "AUTO: aanvoer verandert nog %.1f in 5 min (grens %.1f) - stand %u vasthouden",
                 rise, rise_limit_, stand_);
      rise_holding_ = true;
      dir = 0;
    } else {
      rise_holding_ = false;
    }
  } else {
    rise_holding_ = false;
  }

  // ── Stap-modulatie: max ±1 stand per step_interval (niet volgas) ──
  // Uitzondering: van stilstand naar stand 1 hoeft hoogstens 60 s te wachten.
  // Met een lange interval (bv. 300 s) zou hij anders tot vijf minuten stil
  // blijven staan terwijl er vraag is; de rustige opbouw daarna blijft gewoon
  // gelden. Die 60 s na de vorige stap voorkomt dat hij direct na het
  // afschakelen weer aanslaat.
  uint32_t wacht = (stand_ == 0 && dir > 0 && step_interval_ms_ > 60000) ? 60000 : step_interval_ms_;
  // Omlaag mag sneller, maar nooit naar 0: stoppen blijft de gewone interval houden.
  if (dir < 0 && stand_ > 1 && down_interval_ms_ > 0 && down_interval_ms_ < wacht)
    wacht = down_interval_ms_;
  if (dir != 0 && (now - last_step_ms_) >= wacht) {
    int ns = clamp((int) stand_ + dir, 0, (int) max_stand_);
    if ((uint8_t) ns != stand_) {
      stand_ = (uint8_t) ns;
      last_step_ms_ = now;
      if (std::isnan(sp))
        ESP_LOGI(TAG, "AUTO: geen vraag (%s) -> stand %u", reason_, stand_);
      else if (vroeg_remmen)
        ESP_LOGI(TAG, "AUTO: aanvoer %.1f/%.1f stijgt %.1f/min, verwacht %.1f over %.1f min -> eerder terug naar %u",
                 rs, sp, get_rate_(), rs + get_rate_() * lookahead_min_, lookahead_min_, stand_);
      else
        ESP_LOGI(TAG, "AUTO: aanvoer %.1f/%.1f (fout %.1f) dT %.1f -> stand %u", rs, sp, err, dt, stand_);
    }
  }
  // Plafond vanuit HA verlaagd: direct eronder, niet stap voor stap.
  if (stand_ > max_stand_)
    stand_ = max_stand_;
}

// ═══════════════════════════════════════════════════════════════
//  Setters - loggen alleen als er werkelijk iets verandert. Automations
//  zetten deze vaak elke minuut opnieuw; anders verdrinkt de ene regel die
//  ertoe doet tussen honderden waarin niets gebeurt.
// ═══════════════════════════════════════════════════════════════
void ChofuWP::set_setpoint(float v) {
  const float nieuw = clamp(v, setpoint_min_, setpoint_max_);
  if (fabsf(nieuw - setpoint_) < 0.05f)
    return;
  setpoint_ = nieuw;
  ESP_LOGI(TAG, "Setpoint aanvoer: %.1f°C", setpoint_);
}

void ChofuWP::set_cooling(bool c) {
  if (c == cooling_)
    return;
  cooling_ = c;
  stand_ = 0;          // Altijd via stand 0 wisselen, nooit vliegend omkeren
  last_step_ms_ = 0;   // Mag daarna direct weer opbouwen
  sp_eff_ = NAN;       // Het schuiven begint opnieuw, nu de andere kant op
  ESP_LOGI(TAG, "Bedrijfsmodus: %s", c ? "KOELEN" : "verwarmen");
}

void ChofuWP::set_system_on(bool on) {
  if (on == system_on_)
    return;
  system_on_ = on;
  ESP_LOGI(TAG, "Systeem: %s", on ? "AAN" : "UIT");
}

void ChofuWP::set_mode(Mode m) {
  if (m == mode_)
    return;
  mode_ = m;
  room_i_ = 0.0f;      // De nastelling begint bij elke modewissel opnieuw
  room_i_ms_ = 0;
  room_demand_ = true;
  static const char *const NAMEN[] = {"auto (vast setpoint)", "stooklijn", "kamertemperatuur", "handmatig"};
  ESP_LOGI(TAG, "Modus: %s", NAMEN[m]);
}

void ChofuWP::set_room_temp(float v) {
  room_temp_ = v;
  room_ms_ = millis();
  if (room_ms_ == 0)   // 0 betekent "nooit gezet"
    room_ms_ = 1;
}

void ChofuWP::set_manual_stand(int s) {
  manual_stand_ = clamp(s, 0, (int) max_stand_);
  // Zonder deze melding is een te hoog gevraagde stand onzichtbaar: de slider
  // laat 'm toe, de code knipt 'm af en de WP rapporteert netjes de lagere
  // stand terug - wat makkelijk te lezen is als "de unit kan niet hoger".
  if (s != (int) manual_stand_)
    ESP_LOGW(TAG, "Stand %d gevraagd maar begrensd op %u (Maximale stand).", s, manual_stand_);
}

}  // namespace chofu_wp
}  // namespace esphome
