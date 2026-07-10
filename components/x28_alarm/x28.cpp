#include "x28.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

// ─── Platform-specific direct GPIO access ────────────────────────────────
// Used in ISR (must avoid virtual dispatch + be IRAM-safe) and in the
// timing-critical send_packet() Manchester loop.
#if defined(USE_ESP8266)
  #include <esp8266_peri.h>
  #define MPX_GPIO_READ(pin)   ((GPIO_REG_READ(GPIO_IN_ADDRESS) >> (pin)) & 1)
  #define MPX_W1TS_ADDR        GPIO_OUT_W1TS_ADDRESS
  #define MPX_W1TC_ADDR        GPIO_OUT_W1TC_ADDRESS
  #define MPX_REG_WRITE(a, v)  GPIO_REG_WRITE(a, v)
#elif defined(USE_ESP32)
  #include <soc/gpio_reg.h>
  #define MPX_GPIO_READ(pin)   ((REG_READ(GPIO_IN_REG) >> (pin)) & 1)
  #define MPX_W1TS_ADDR        GPIO_OUT_W1TS_REG
  #define MPX_W1TC_ADDR        GPIO_OUT_W1TC_REG
  #define MPX_REG_WRITE(a, v)  REG_WRITE(a, v)
#endif

namespace esphome {
namespace x28_alarm {

static const char *const TAG = "x28_alarm";

// ─── Keyboard code lookup table ─────────────────────────────────────────
const uint16_t KEYBOARD_CODES[] = {
    0x0000, 0x8013, 0x8025, 0x0036, 0x8046, 0x0055, 0x0063, 0x8070,
    0x8089, 0x009A, 0x00AC, 0x80BF, 0x00CF, 0x80DC, 0x80EA, 0x00F9,
    0x00AC, 0x810A, 0x80BF, 0x813C, 0x00F9, 0x0119, 0x80EA, 0x012F,
    0x8169,
};

// ─── MPXBus static instance ─────────────────────────────────────────────
MPXBus *MPXBus::instance_ = nullptr;

void MPXBus::setup(InternalGPIOPin *rx_pin, InternalGPIOPin *tx_pin,
                   bool invert_rx, bool invert_tx) {
  instance_ = this;
  rx_pin_ = rx_pin;
  tx_pin_ = tx_pin;
  invert_rx_ = invert_rx;
  invert_tx_ = invert_tx;
  rx_pin_num_ = rx_pin->get_pin();
  tx_pin_num_ = tx_pin->get_pin();
  rx_idle_level_ = !invert_rx;

  rx_pin_->setup();
  tx_pin_->setup();

  attachInterrupt(digitalPinToInterrupt(rx_pin_num_),
                  interrupt_handler, CHANGE);
}

void MPXBus::loop() {
  while (!buffer_.is_empty()) {
    MPXPacket packet(buffer_.read());
    if (!packet.is_valid())
      continue;

    if (event_callback_)
      event_callback_(packet.get_word());
  }
}

void IRAM_ATTR MPXBus::interrupt_handler() {
  if (!instance_) return;

  uint32_t now = micros();
  uint32_t length = now - instance_->prev_micros_;

  #if defined(USE_ESP8266) || defined(USE_ESP32)
    bool pin_level = MPX_GPIO_READ(instance_->rx_pin_num_);
  #else
    bool pin_level = instance_->rx_pin_->digital_read();
  #endif

  // Process on the ACTIVE bus level: length between consecutive active edges
  // gives the IDLE pulse width, which correctly maps to Manchester bits
  // (long idle = bit 1, short idle = bit 0).
  if (pin_level != instance_->rx_idle_level_) {
    if (length > IDLE_TIME) {
      instance_->recbuf_ = 0;
      instance_->bit_number_ = 0;
    } else {
      instance_->recbuf_ = (instance_->recbuf_ << 1);
      if (length > ZERO_TIME)
        instance_->recbuf_ |= 1;

      if (++instance_->bit_number_ == 16) {
        instance_->buffer_.push(instance_->recbuf_);
        instance_->recbuf_ = 0;
        instance_->bit_number_ = 0;
      }
    }
  }
  instance_->prev_micros_ = now;
}

void MPXBus::send_packet(uint16_t payload) {
  // CTS wait: bus must be idle for CTS_TIME before transmitting
  uint32_t bus_idle_start = micros();
  while (micros() - bus_idle_start < CTS_TIME) {
    #if defined(USE_ESP8266) || defined(USE_ESP32)
      bool active = MPX_GPIO_READ(rx_pin_num_);
    #else
      bool active = rx_pin_->digital_read();
    #endif
    if (active != rx_idle_level_) {
      bus_idle_start = micros();
    }
    delay(0);
  }

  // Detach RX interrupt during TX to avoid false triggers from our own signal
  detachInterrupt(digitalPinToInterrupt(rx_pin_num_));
  tx_pin_->pin_mode(gpio::FLAG_OUTPUT);

  #if defined(USE_ESP8266) || defined(USE_ESP32)
    uint32_t pin_mask = 1 << tx_pin_num_;
    uint32_t reg_idle = invert_tx_ ? (uint32_t)MPX_W1TC_ADDR : (uint32_t)MPX_W1TS_ADDR;
    uint32_t reg_active = invert_tx_ ? (uint32_t)MPX_W1TS_ADDR : (uint32_t)MPX_W1TC_ADDR;

    // Start bit: bus LOW (active)
    MPX_REG_WRITE(reg_active, pin_mask);
    delayMicroseconds(BIT_TIME);

    for (int i = 0; i < 16; i++) {
      if (!(payload & 0x8000)) {
        // '0' on bus: idle for 1T, active for 2T
        MPX_REG_WRITE(reg_idle, pin_mask);
        delayMicroseconds(BIT_TIME);
        MPX_REG_WRITE(reg_active, pin_mask);
        delayMicroseconds(2 * BIT_TIME);
      } else {
        // '1' on bus: idle for 2T, active for 1T
        MPX_REG_WRITE(reg_idle, pin_mask);
        delayMicroseconds(2 * BIT_TIME);
        MPX_REG_WRITE(reg_active, pin_mask);
        delayMicroseconds(BIT_TIME);
      }
      payload <<= 1;
    }

    // Stop bit: bus HIGH (idle)
    MPX_REG_WRITE(reg_idle, pin_mask);
  #else
    tx_pin_->digital_write(invert_tx_ ? HIGH : LOW);
    delayMicroseconds(BIT_TIME);

    for (int i = 0; i < 16; i++) {
      if (!(payload & 0x8000)) {
        tx_pin_->digital_write(invert_tx_ ? LOW : HIGH);
        delayMicroseconds(BIT_TIME);
        tx_pin_->digital_write(invert_tx_ ? HIGH : LOW);
        delayMicroseconds(2 * BIT_TIME);
      } else {
        tx_pin_->digital_write(invert_tx_ ? LOW : HIGH);
        delayMicroseconds(2 * BIT_TIME);
        tx_pin_->digital_write(invert_tx_ ? HIGH : LOW);
        delayMicroseconds(BIT_TIME);
      }
      payload <<= 1;
    }

    tx_pin_->digital_write(invert_tx_ ? LOW : HIGH);
  #endif
  delayMicroseconds(BIT_TIME);
  tx_pin_->pin_mode(gpio::FLAG_INPUT);
  attachInterrupt(digitalPinToInterrupt(rx_pin_num_), interrupt_handler, CHANGE);
}

void MPXBus::send_key(uint8_t key_index) {
  if (key_index < sizeof(KEYBOARD_CODES) / sizeof(KEYBOARD_CODES[0]))
    send_packet(KEYBOARD_CODES[key_index]);
}

void MPXBus::send_keys(const std::string &keys) {
  send_keys(keys.data(), keys.size());
}

void MPXBus::send_keys(const char *keys, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = keys[i];
    if (c >= '0' && c <= '9')
      send_key(c - '0');
    else if (c == 'P')
      send_key(10);
    else if (c == 'p') {
      send_packet(0x00AC);
      delay(KEY_DELAY_MS);
      send_packet(0x810A);
    } else if (c == 'F')
      send_key(11);
    else if (c == 'f') {
      send_packet(0x80BF);
      delay(KEY_DELAY_MS);
      send_packet(0x813C);
    } else if (c == 'M')
      send_key(13);
    else if (c == 'Z')
      send_packet(0x00CF);
    else if (c == 'L') {
      send_packet(0x00CF);
      delay(KEY_DELAY_MS);
      send_packet(0x8169);
    } else if (c == '!')
      send_key(14);
    else if (c == '@')
      send_key(15);
    else if (c == '#') {
      send_packet(0x80EA);
      delay(KEY_DELAY_MS);
      send_packet(0x012F);
    } else if (c == '*') {
      send_packet(0x00F9);
      delay(KEY_DELAY_MS);
      send_packet(0x0119);
    }
    delay(KEY_DELAY_MS);
  }
}

bool MPXBus::is_keyboard_code(uint16_t code) {
  for (size_t i = 0; i < sizeof(KEYBOARD_CODES) / sizeof(uint16_t); i++) {
    if (KEYBOARD_CODES[i] == code)
      return true;
  }
  return false;
}

// ─── X28Alarm ───────────────────────────────────────────────────────────
void X28Alarm::setup() {
  model_capabilities_ = get_model_capabilities(model_);
  bus_.setup(rx_pin_, tx_pin_, invert_rx_, invert_tx_);

  bus_.set_event_callback([this](uint16_t word) {
    this->on_event(word);
  });

  register_service([this](const std::string &keys) { this->send_keys_service(keys); },
                   "send_keys", {"keys"});

  register_service([this] { this->enter_programming_service(); }, "enter_programming");
  register_service([this] { this->enter_advanced_service(); }, "enter_advanced_programming");
  register_service([this] { this->exit_programming_service(); }, "exit_programming");
  register_service([this](int seconds) { this->set_entry_delay_service(seconds); }, "set_entry_delay", {"seconds"});
  register_service([this](int seconds) { this->set_exit_delay_service(seconds); }, "set_exit_delay", {"seconds"});
  register_service([this](int minutes) { this->set_siren_duration_service(minutes); }, "set_siren_duration", {"minutes"});
  register_service([this](int minutes) { this->set_siren_b_duration_service(minutes); }, "set_siren_b_duration", {"minutes"});
  register_service([this](bool enabled) { this->set_sabotage_inhibit_service(enabled); }, "set_sabotage_inhibit", {"enabled"});
  register_service([this](int hz) { this->set_ac_frequency_service(hz); }, "set_ac_frequency", {"hz"});
  register_service([this](bool enabled) { this->set_entry_annunciator_service(enabled); }, "set_entry_annunciator", {"enabled"});
  register_service([this](bool enabled) { this->set_battery_save_service(enabled); }, "set_battery_save", {"enabled"});
  register_service([this](bool disarm_only) { this->set_owner_code_condition_service(disarm_only); }, "set_owner_code_condition", {"disarm_only"});
  register_service([this](bool enabled) { this->set_zone_conditionality_service(enabled); }, "set_zone_conditionality", {"enabled"});
  register_service([this](const std::string &new_code) { this->change_owner_code_service(new_code); }, "change_owner_code", {"new_code"});
  register_service([this](const std::string &new_code) { this->change_installer_code_service(new_code); }, "change_installer_code", {"new_code"});
  register_service([this](int user, const std::string &code, int permissions, bool can_disarm) { this->program_user_service(user, code, permissions, can_disarm); }, "program_user", {"user", "code", "permissions", "can_disarm"});
  if (model_capabilities_.has_rf_learning) {
    register_service([this] { this->rf_learn_mode_service(); }, "rf_learn_mode");
    register_service([this](int slot) { this->rf_learn_slot_service(slot); }, "rf_learn_slot", {"slot"});
    register_service([this](int slot) { this->rf_delete_slot_service(slot); }, "rf_delete_slot", {"slot"});
    register_service([this] { this->exit_rf_learning_service(); }, "exit_rf_learning");
  }
  register_service([this](int zone) { this->toggle_zone_in_mode_service(zone); }, "toggle_zone_in_mode", {"zone"});
  register_service([this] { this->save_estoy_config_service(); }, "save_estoy_config");
  register_service([this] { this->save_mevoy_config_service(); }, "save_mevoy_config");
  register_service([this](int zone, const std::string &type) { this->set_zone_type_service(zone, type); }, "set_zone_type", {"zone", "type"});
  register_service([this] { this->set_panic_zone_service(); }, "set_panic_zone");
  register_service([this] { this->set_tamper_zone_service(); }, "set_tamper_zone");
  register_service([this](bool crystal) { this->set_clock_source_service(crystal); }, "set_clock_source", {"crystal"});
  if (model_capabilities_.max_wired_zones > 0) {
    register_service([this](bool enabled) { this->set_wired_zones_service(enabled); }, "set_wired_zones", {"enabled"});
  }
  register_service([this](bool enabled) { this->set_partition_merge_service(enabled); }, "set_partition_merge", {"enabled"});
  register_service([this](int output, int option, int partition) { this->set_pgm_output_service(output, option, partition); }, "set_pgm_output", {"output", "option", "partition"});

  ESP_LOGCONFIG(TAG, "X-28 Alarm initialized");
}

void X28Alarm::loop() {
  bus_.loop();

  for (auto &vz : virtual_zones_) {
    bool state = vz.sensor->state;
    if (state != vz.last_state) {
      vz.last_state = state;
      bool trigger = vz.trigger_on_open ? !state : state;
      if (trigger) {
        bus_.send_packet(vz.packet_code);
      } else if (vz.clear_on_close) {
        bus_.send_packet(vz.packet_code);
      }
    }
  }

  if (arm_pending_) {
    if (millis() - arm_pending_start_ > ARM_TIMEOUT_MS) {
      arm_pending_ = false;
      if (disarm_pending_) {
        disarm_pending_ = false;
        if (acp_)
          acp_->publish_state(alarm_control_panel::ACP_STATE_DISARMED);
        ESP_LOGW(TAG, "Disarm timeout — no ALARM_DISARMED received");
      } else {
        if (acp_)
          acp_->publish_state(alarm_control_panel::ACP_STATE_DISARMED);
        ESP_LOGW(TAG, "Arm timeout — no ALARM_ARMED received");
      }
    }
  }

  for (int i = 0; i < MAX_ZONES; i++) {
    if (zone_sensors_[i] != nullptr && zone_last_packet_[i] != 0) {
      if (millis() - zone_last_packet_[i] > zone_debounce_ms_) {
        zone_sensors_[i]->publish_state(false);
        zone_last_packet_[i] = 0;
      }
    }
  }
}

void X28Alarm::dump_config() {
  ESP_LOGCONFIG(TAG, "X-28 Alarm:");
  ESP_LOGCONFIG(TAG, "  Model: %s", model_ == X28Model::AUTO ? "AUTO" :
                model_ == X28Model::N4_MPXH ? "N4-MPXH" :
                model_ == X28Model::N8_MPXH ? "N8-MPXH" :
                model_ == X28Model::N8F_MPXH ? "N8F-MPXH" :
                model_ == X28Model::N16_MPXH ? "N16-MPXH" :
                model_ == X28Model::N32_MPXH ? "N32-MPXH" :
                model_ == X28Model::N32F_MPXH ? "N32F-MPXH" :
                "OTHER");
  ESP_LOGCONFIG(TAG, "  Zones: %d MPXH, %d wired",
                model_capabilities_.max_mpxh_zones,
                model_capabilities_.max_wired_zones);
  ESP_LOGCONFIG(TAG, "  Wireless: %s, RF Learning: %s",
                model_capabilities_.has_wireless ? "YES" : "NO",
                model_capabilities_.has_rf_learning ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  RX Pin: GPIO%d (invert: %s)",
                rx_pin_->get_pin(), invert_rx_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  TX Pin: GPIO%d (invert: %s)",
                tx_pin_->get_pin(), invert_tx_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Code: %s, Installer Code: %s",
                code_.c_str(), installer_code_.c_str());
  ESP_LOGCONFIG(TAG, "  Debug: %s, Sniffing: %s",
                debug_ ? "YES" : "NO", sniffing_enabled_ ? "YES" : "NO");
  ESP_LOGCONFIG(TAG, "  Zone debounce: %d ms", zone_debounce_ms_);
  ESP_LOGCONFIG(TAG, "  Virtual zones: %d", (int)virtual_zones_.size());
}

void X28Alarm::set_zone_sensor(uint8_t zone, binary_sensor::BinarySensor *s) {
  if (zone > 0 && zone <= MAX_ZONES)
    zone_sensors_[zone - 1] = s;
}

void X28Alarm::add_virtual_zone(uint8_t zone, uint16_t packet_code,
                                 bool clear_on_close, bool trigger_on_open,
                                 binary_sensor::BinarySensor *sensor) {
  virtual_zones_.push_back({zone, packet_code, clear_on_close, sensor, trigger_on_open, false});
}

void X28Alarm::send_keys_service(const std::string &keys) {
  bus_.send_keys(keys);
}

// ─── Programming Service Helper ──────────────────────────────────────────
void X28Alarm::send_programmed_sequence(const std::string &body, bool use_installer, bool advanced, bool exit_f) {
  const std::string &prefix = use_installer ? installer_code_ : code_;
  char buf[48];
  size_t pos = 0;
  memcpy(buf + pos, prefix.data(), prefix.size());
  pos += prefix.size();
  buf[pos++] = 'P';
  buf[pos++] = 'P';
  if (advanced)
    buf[pos++] = 'p';
  memcpy(buf + pos, body.data(), body.size());
  pos += body.size();
  if (exit_f)
    buf[pos++] = 'F';
  bus_.send_keys(buf, pos);
}

// ─── High-Level Programming Services ────────────────────────────────────
void X28Alarm::enter_programming_service() {
  send_programmed_sequence("", false, false, false);
}

void X28Alarm::enter_advanced_service() {
  send_programmed_sequence("", false, true, false);
}

void X28Alarm::exit_programming_service() {
  bus_.send_key(11);
}

void X28Alarm::set_entry_delay_service(int seconds) {
  if (seconds < 5 || seconds > 99) {
    ESP_LOGW(TAG, "Entry delay %d out of range (5-99), clamping", seconds);
    seconds = (seconds < 5) ? 5 : 99;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "P881%02d", seconds);
  send_programmed_sequence(buf, true, true);
}

void X28Alarm::set_exit_delay_service(int seconds) {
  if (seconds < 15 || seconds > 99) {
    ESP_LOGW(TAG, "Exit delay %d out of range (15-99), clamping", seconds);
    seconds = (seconds < 15) ? 15 : 99;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "P882%02d", seconds);
  send_programmed_sequence(buf, true, true);
}

void X28Alarm::set_siren_duration_service(int minutes) {
  if (minutes < 1 || minutes > 12) {
    ESP_LOGW(TAG, "Siren duration %d out of range (1-12), clamping", minutes);
    minutes = (minutes < 1) ? 1 : 12;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "P883%02d", minutes);
  send_programmed_sequence(buf, true, true);
}

void X28Alarm::set_siren_b_duration_service(int minutes) {
  if (minutes < 1 || minutes > 12) {
    ESP_LOGW(TAG, "Siren B duration %d out of range (1-12), clamping", minutes);
    minutes = (minutes < 1) ? 1 : 12;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "P775%02d", minutes);
  send_programmed_sequence(buf, true, true);
}

void X28Alarm::set_sabotage_inhibit_service(bool enabled) {
  const char body[] = {'P', '7', '7', '4', enabled ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::set_ac_frequency_service(int hz) {
  if (hz != 50 && hz != 60) {
    ESP_LOGW(TAG, "Invalid AC frequency %d, using 50", hz);
    hz = 50;
  }
  const char body[] = {'P', '7', '7', '3', hz == 60 ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::set_entry_annunciator_service(bool enabled) {
  const char body[] = {'P', '7', '7', '6', enabled ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::set_battery_save_service(bool enabled) {
  const char body[] = {'P', '8', '8', '6', enabled ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::set_owner_code_condition_service(bool disarm_only) {
  const char body[] = {'P', '8', '8', '0', disarm_only ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::set_zone_conditionality_service(bool enabled) {
  const char body[] = {'P', '8', '8', '4', enabled ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::change_owner_code_service(const std::string &new_code) {
  std::string seq = installer_code_ + "PP" + new_code;
  bus_.send_keys(seq);
  code_ = new_code;
}

void X28Alarm::change_installer_code_service(const std::string &new_code) {
  std::string seq = installer_code_ + "PPpP889" + new_code + "F";
  bus_.send_keys(seq);
  installer_code_ = new_code;
}

void X28Alarm::program_user_service(int user, const std::string &code, int permissions, bool can_disarm) {
  if (user < 1 || user > 30) {
    ESP_LOGW(TAG, "Invalid user %d, must be 1-30", user);
    return;
  }
  size_t code_len = code.size();
  if (code_len < 4 || code_len > 6) {
    ESP_LOGW(TAG, "Invalid code length %d, must be 4-6 digits", code_len);
    return;
  }
  if (permissions < 0 || permissions > 4) {
    ESP_LOGW(TAG, "Invalid permissions %d, must be 0-4", permissions);
    return;
  }
  std::string seq = code_ + "PPF2633P";
  char buf[4];
  snprintf(buf, sizeof(buf), "%02d", user);
  seq += buf;
  snprintf(buf, sizeof(buf), "%02d", (int)code_len);
  seq += buf;
  seq += code;
  seq += '0' + permissions;
  seq += can_disarm ? '1' : '0';
  seq += 'F';
  bus_.send_keys(seq);
}

void X28Alarm::rf_learn_mode_service() {
  std::string seq = code_ + "PPF2337";
  bus_.send_keys(seq);
}

void X28Alarm::rf_learn_slot_service(int slot) {
  if (slot < 2 || slot > 32) {
    ESP_LOGW(TAG, "Invalid RF slot %d, must be 2-32", slot);
    return;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "P%02d", slot);
  bus_.send_keys(std::string(buf));
}

void X28Alarm::rf_delete_slot_service(int slot) {
  if (slot < 2 || slot > 32) {
    ESP_LOGW(TAG, "Invalid RF slot %d, must be 2-32", slot);
    return;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "P%02d0", slot);
  bus_.send_keys(std::string(buf));
}

void X28Alarm::exit_rf_learning_service() {
  bus_.send_key(11);
}

void X28Alarm::toggle_zone_in_mode_service(int zone) {
  if (zone < 1 || zone > model_capabilities_.max_mpxh_zones) {
    ESP_LOGW(TAG, "Zone %d out of range (1-%d)", zone, model_capabilities_.max_mpxh_zones);
    return;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "Z%02d", zone);
  bus_.send_keys(std::string(buf));
}

void X28Alarm::save_estoy_config_service() {
  send_programmed_sequence("P7781", false, true);
}

void X28Alarm::save_mevoy_config_service() {
  send_programmed_sequence("P7782", false, true);
}

void X28Alarm::set_zone_type_service(int zone, const std::string &type) {
  if (zone < 1 || zone > model_capabilities_.max_mpxh_zones) {
    ESP_LOGW(TAG, "Zone %d out of range (1-%d)", zone, model_capabilities_.max_mpxh_zones);
    return;
  }
  int pcode;
  if (type == "output_b")                     pcode = 991;
  else if (type == "output_ab")               pcode = 992;
  else if (type == "fire")                    pcode = 993;
  else if (type == "normal" || type == "robbery") pcode = 994;
  else if (type == "24h_protection")          pcode = 995;
  else if (type == "fast_robbery")            pcode = 996;
  else return;

  char buf[16];
  snprintf(buf, sizeof(buf), "P%d%02d", pcode, zone);
  send_programmed_sequence(buf, true, true);
}

void X28Alarm::set_panic_zone_service() {
  send_programmed_sequence("P997", true, true);
}

void X28Alarm::set_tamper_zone_service() {
  send_programmed_sequence("P998", true, true);
}

void X28Alarm::set_clock_source_service(bool crystal) {
  const char body[] = {'P', '7', '7', '7', crystal ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::set_wired_zones_service(bool enabled) {
  const char body[] = {'P', '8', '8', '5', enabled ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::set_partition_merge_service(bool enabled) {
  const char body[] = {'P', '8', '8', '8', enabled ? '1' : '0'};
  send_programmed_sequence(std::string(body, 5), true, true);
}

void X28Alarm::set_pgm_output_service(int output, int option, int partition) {
  if (output < 0 || output > 2) {
    ESP_LOGW(TAG, "PGM output %d out of range (0-2), clamping", output);
    output = (output < 0) ? 0 : 2;
  }
  if (option < 0 || option > 9) {
    ESP_LOGW(TAG, "PGM option %d out of range (0-9), clamping", option);
    option = (option < 0) ? 0 : 9;
  }
  if (partition < 1 || partition > 8) {
    ESP_LOGW(TAG, "Partition %d out of range (1-8), clamping", partition);
    partition = (partition < 1) ? 1 : 8;
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "P77%d%d%d", output, option, partition);
  send_programmed_sequence(buf, true, true);
}

// ─── Mode Toggle ────────────────────────────────────────────────────────
void X28Alarm::toggle_mode(uint16_t target_mode) {
  if (last_mode_ == target_mode)
    return;
  for (int attempt = 0; attempt < MODE_TOGGLE_ATTEMPTS; attempt++) {
    bus_.send_key(13);
    mode_waiting_ = true;
    uint32_t start = millis();
    while (mode_waiting_ && millis() - start < MODE_TOGGLE_WAIT_MS) {
      bus_.loop();
      delay(0);
    }
    if (!mode_waiting_)
      return;
  }
  ESP_LOGW(TAG, "Mode toggle timeout after %d attempts — proceeding anyway", MODE_TOGGLE_ATTEMPTS);
  mode_waiting_ = false;
}

// ─── HA Actions ─────────────────────────────────────────────────────────
void X28Alarm::arm_away() {
  toggle_mode(MPX_CODE_ME_VOY);
  bus_.send_keys(code_);
  arm_pending_start_ = millis();
  arm_pending_ = true;
  disarm_pending_ = false;
  last_mode_ = MPX_CODE_ME_VOY;
  if (acp_)
    acp_->publish_state(alarm_control_panel::ACP_STATE_ARMING);
}

void X28Alarm::arm_home() {
  toggle_mode(MPX_CODE_ESTOY);
  bus_.send_keys(code_);
  arm_pending_start_ = millis();
  arm_pending_ = true;
  disarm_pending_ = false;
  last_mode_ = MPX_CODE_ESTOY;
  if (acp_)
    acp_->publish_state(alarm_control_panel::ACP_STATE_ARMING);
}

void X28Alarm::disarm() {
  bus_.send_keys(code_);
  arm_pending_start_ = millis();
  arm_pending_ = true;
  disarm_pending_ = true;
  if (acp_)
    acp_->publish_state(alarm_control_panel::ACP_STATE_ARMING);
}

// ─── Event Handler ──────────────────────────────────────────────────────
void X28Alarm::on_event(uint16_t word) {
  if (sniffing_enabled_) {
    uint32_t now = millis();
    if (word != last_sniff_word_ ||
        (now - last_sniff_log_) > sniffing_throttle_ms_) {
      last_sniff_word_ = word;
      last_sniff_log_ = now;
      MPXPacket p(word);
      ESP_LOGD(TAG, "PKT 0x%04X P=%d ID=%d DATA=0x%02X CS=%d",
               p.get_word(), p.get_parity(), p.get_id(), p.get_data(),
               p.get_checksum());
      if (sniffer_text_)
        sniffer_text_->publish_state(str_sprintf("0x%04X", word));
    }
  }

  if (bus_.is_keyboard_code(word)) {
    ESP_LOGV(TAG, "KBD code 0x%04X", word);
    return;
  }

  switch (word) {
    case MPX_CODE_ALARM_ARMED:
      armed_confirmed_ = true;
      arm_pending_ = false;
      disarm_pending_ = false;
      if (acp_)
        acp_->publish_state(
            last_mode_ == MPX_CODE_ESTOY
                ? alarm_control_panel::ACP_STATE_ARMED_HOME
                : alarm_control_panel::ACP_STATE_ARMED_AWAY);
      return;

    case MPX_CODE_ALARM_DISARMED:
      armed_confirmed_ = false;
      arm_pending_ = false;
      disarm_pending_ = false;
      if (acp_)
        acp_->publish_state(alarm_control_panel::ACP_STATE_DISARMED);
      if (estoy_sensor_)
        estoy_sensor_->publish_state(false);
      return;

    case MPX_CODE_ESTOY:
      last_mode_ = MPX_CODE_ESTOY;
      mode_waiting_ = false;
      if (estoy_sensor_)
        estoy_sensor_->publish_state(true);
      return;

    case MPX_CODE_ME_VOY:
      last_mode_ = MPX_CODE_ME_VOY;
      mode_waiting_ = false;
      if (estoy_sensor_)
        estoy_sensor_->publish_state(false);
      return;
  }

  on_packet(word);
}

static uint16_t mpxh_code_for_zone(uint8_t zone) {
  static const uint8_t cs[] = {5, 3, 0, 0};
  uint16_t w = (1 << 12) | ((uint16_t)(0x60 + zone) << 4) | cs[(zone - 1) & 3];
  if (__builtin_popcount(w) & 1) w |= 0x8000;
  return w;
}

void X28Alarm::on_packet(uint16_t word) {
  static const uint16_t WIRED_CODES[] = {
      MPX_CODE_Z1_WIRED, MPX_CODE_Z2_WIRED, MPX_CODE_Z3_WIRED, MPX_CODE_Z4_WIRED,
      MPX_CODE_Z5_WIRED, MPX_CODE_Z6_WIRED, MPX_CODE_Z7_WIRED, MPX_CODE_Z8_WIRED,
  };
  for (uint8_t z = 0; z < model_capabilities_.max_mpxh_zones; z++) {
    uint16_t override = zone_code_overrides_[z + 1];
    if (word == mpxh_code_for_zone(z + 1) ||
        (z < model_capabilities_.max_wired_zones && word == WIRED_CODES[z]) ||
        (override != 0 && word == override)) {
      on_zone(z + 1, true);
      return;
    }
  }
}

void X28Alarm::on_zone(uint8_t zone, bool triggered) {
  if (zone == 0 || zone > MAX_ZONES) return;

  if (triggered) {
    zone_last_packet_[zone - 1] = millis();
    if (zone_sensors_[zone - 1] != nullptr)
      zone_sensors_[zone - 1]->publish_state(true);
  }

  if (armed_confirmed_ && triggered) {
    if (acp_)
      acp_->publish_state(alarm_control_panel::ACP_STATE_TRIGGERED);
  }
}

// ─── X28AlarmControlPanel ────────────────────────────────────────────────
bool X28AlarmControlPanel::arm_away() {
  if (parent_)
    parent_->arm_away();
  return true;
}

bool X28AlarmControlPanel::arm_home() {
  if (parent_)
    parent_->arm_home();
  return true;
}

bool X28AlarmControlPanel::disarm() {
  if (parent_)
    parent_->disarm();
  return true;
}

// ─── X28ActionButton ─────────────────────────────────────────────────────
void X28ActionButton::press_action() {
  if (!parent_) return;
  parent_->send_packet(code_);
  if (long_code_ != 0) {
    delay(LONG_PRESS_DELAY_MS);
    parent_->send_packet(long_code_);
  }
}

}  // namespace x28_alarm
}  // namespace esphome
