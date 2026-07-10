#pragma once

#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/log.h"
#include "esphome/components/alarm_control_panel/alarm_control_panel.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace x28_alarm {

// ─── Timing ────────────────────────────────────────────────────────────
constexpr uint32_t BIT_TIME = 1270;
constexpr uint32_t ZERO_TIME = 2000;
constexpr uint32_t IDLE_TIME = 5000;
constexpr uint32_t CTS_TIME = 25000;
constexpr uint8_t MPX_BUFFER_LENGTH = 64;
constexpr uint8_t MAX_ZONES = 32;
constexpr uint32_t ARM_TIMEOUT_MS = 10000;
constexpr uint32_t MODE_TOGGLE_WAIT_MS = 1000;
constexpr uint8_t MODE_TOGGLE_ATTEMPTS = 10;
constexpr uint32_t KEY_DELAY_MS = 150;
constexpr uint32_t LONG_PRESS_DELAY_MS = 500;

// ─── Model ─────────────────────────────────────────────────────────────
enum class X28Model : uint8_t {
  AUTO = 0,
  N4_MPXH,
  N8_MPXH,
  N8F_MPXH,
  N16_MPXH,
  N32_MPXH,
  N32F_MPXH,
  _9002_MPX,
  _9003_MPX,
  _9004_MPX,
};

struct ModelCapabilities {
  uint8_t max_mpxh_zones;
  uint8_t max_wired_zones;
  bool has_wireless;
  bool has_rf_learning;
};

constexpr ModelCapabilities get_model_capabilities(X28Model model) {
  switch (model) {
    case X28Model::N4_MPXH:   return { 4,  4,  false, false };
    case X28Model::N8_MPXH:   return { 8,  8,  true,  false };
    case X28Model::N8F_MPXH:  return { 8,  0,  false, false };
    case X28Model::N16_MPXH:  return { 16, 8,  true,  true  };
    case X28Model::N32_MPXH:  return { 32, 8,  true,  true  };
    case X28Model::N32F_MPXH: return { 32, 0,  false, true  };
    case X28Model::_9002_MPX: return { 2,  2,  false, false };
    case X28Model::_9003_MPX: return { 3,  3,  false, false };
    case X28Model::_9004_MPX: return { 4,  4,  false, false };
    case X28Model::AUTO:
    default:                  return { 32, 8,  true,  true  };
  }
}

// ─── Packet codes ──────────────────────────────────────────────────────
enum MPXCode : uint16_t {
  MPX_CODE_KEY_0           = 0x0000,
  MPX_CODE_KEY_1           = 0x8013,
  MPX_CODE_KEY_2           = 0x8025,
  MPX_CODE_KEY_3           = 0x0036,
  MPX_CODE_KEY_4           = 0x8046,
  MPX_CODE_KEY_5           = 0x0055,
  MPX_CODE_KEY_6           = 0x0063,
  MPX_CODE_KEY_7           = 0x8070,
  MPX_CODE_KEY_8           = 0x8089,
  MPX_CODE_KEY_9           = 0x009A,
  MPX_CODE_KEY_P           = 0x00AC,
  MPX_CODE_KEY_P_LONG      = 0x810A,
  MPX_CODE_KEY_F           = 0x80BF,
  MPX_CODE_KEY_F_LONG      = 0x813C,
  MPX_CODE_KEY_ZONA_IN     = 0x00CF,
  MPX_CODE_KEY_ZONA_OUT    = 0x8169,
  MPX_CODE_KEY_MODO        = 0x80DC,
  MPX_CODE_KEY_PANIC       = 0x80EA,
  MPX_CODE_KEY_PANIC_LONG  = 0x012F,
  MPX_CODE_KEY_FIRE        = 0x00F9,
  MPX_CODE_KEY_FIRE_LONG   = 0x0119,

  MPX_CODE_ALARM_ARMED     = 0x49C1,
  MPX_CODE_ALARM_DISARMED  = 0xC92B,
  MPX_CODE_ESTOY           = 0x4BE8,
  MPX_CODE_ME_VOY          = 0xCBAE,

  MPX_CODE_Z1_MPXH         = 0x1615,
  MPX_CODE_Z2_MPXH         = 0x1623,
  MPX_CODE_Z3_MPXH         = 0x9630,
  MPX_CODE_Z4_MPXH         = 0x1640,
  MPX_CODE_Z5_MPXH         = 0x1655,
  MPX_CODE_Z6_MPXH         = 0x1663,
  MPX_CODE_Z7_MPXH         = 0x9670,
  MPX_CODE_Z8_MPXH         = 0x1680,

  MPX_CODE_Z1_WIRED        = 0xB08A,
  MPX_CODE_Z2_WIRED        = 0xB045,
  MPX_CODE_Z3_WIRED        = 0xB026,
  MPX_CODE_Z4_WIRED        = 0xB010,
  MPX_CODE_Z5_WIRED        = 0xB008,
  MPX_CODE_Z6_WIRED        = 0xB1D2,
  MPX_CODE_Z7_WIRED        = 0xB05C,
  MPX_CODE_Z8_WIRED        = 0xB134,
};

// ─── MPXPacket ─────────────────────────────────────────────────────────
class MPXPacket {
 public:
  explicit MPXPacket(uint16_t word) : word_(word) {}

  uint16_t get_word() const { return word_; }
  uint16_t get_parity() const { return (word_ >> 15) & 0x01; }
  uint16_t get_id() const { return (word_ >> 12) & 0x07; }
  uint16_t get_data() const { return (word_ >> 4) & 0xFF; }
  uint16_t get_checksum() const { return word_ & 0x0F; }

  bool is_valid() const {
    return (__builtin_popcount(word_) % 2) == 0;
  }

 private:
  uint16_t word_;
};

// ─── CircularBuffer ────────────────────────────────────────────────────
class CircularBuffer {
 public:
  void push(uint16_t word) {
    uint8_t next = (write_index_ + 1) & (MPX_BUFFER_LENGTH - 1);
    if (next == read_index_) return;
    buffer_[write_index_] = word;
    write_index_ = next;
  }

  uint16_t read() {
    if (is_empty()) return 0;
    uint16_t val = buffer_[read_index_];
    read_index_ = (read_index_ + 1) & (MPX_BUFFER_LENGTH - 1);
    return val;
  }

  bool is_empty() const { return read_index_ == write_index_; }
  bool is_full() const { return ((write_index_ + 1) & (MPX_BUFFER_LENGTH - 1)) == read_index_; }

 private:
  uint16_t buffer_[MPX_BUFFER_LENGTH];
  volatile uint8_t write_index_ = 0;
  volatile uint8_t read_index_ = 0;
};

// ─── Forward declarations ──────────────────────────────────────────────
class X28Alarm;

// ─── MPXBus ────────────────────────────────────────────────────────────
class MPXBus {
 public:
  void setup(InternalGPIOPin *rx_pin, InternalGPIOPin *tx_pin,
             bool invert_rx, bool invert_tx);
  void loop();
  void send_packet(uint16_t payload);
  void send_key(uint8_t key_index);
  void send_keys(const std::string &keys);
  void send_keys(const char *keys, size_t len);

  void set_event_callback(std::function<void(uint16_t)> callback) {
    event_callback_ = callback;
  }

  bool is_keyboard_code(uint16_t code);

 protected:
  static void interrupt_handler();

  InternalGPIOPin *rx_pin_{nullptr};
  InternalGPIOPin *tx_pin_{nullptr};
  bool invert_rx_{true};
  bool invert_tx_{true};
  uint8_t rx_pin_num_{0};
  uint8_t tx_pin_num_{0};
  bool rx_idle_level_{false};

  CircularBuffer buffer_;
  uint16_t recbuf_{0};
  uint8_t bit_number_{0};
  uint32_t prev_micros_{0};

  std::function<void(uint16_t)> event_callback_;

  static MPXBus *instance_;
};

// ─── X28Alarm ──────────────────────────────────────────────────────────
class X28Alarm : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  // ── Configuration ──
  void set_rx_pin(InternalGPIOPin *pin) { rx_pin_ = pin; }
  void set_tx_pin(InternalGPIOPin *pin) { tx_pin_ = pin; }
  void set_code(const std::string &code) { code_ = code; }
  void set_installer_code(const std::string &code) { installer_code_ = code; }
  void set_model(X28Model model) { model_ = model; }
  void set_invert_rx(bool inv) { invert_rx_ = inv; }
  void set_invert_tx(bool inv) { invert_tx_ = inv; }
  void set_debug(bool debug) { debug_ = debug; }
  void set_sniffing_enabled(bool en) { sniffing_enabled_ = en; }
  void set_sniffing_throttle(uint32_t ms) { sniffing_throttle_ms_ = ms; }
  void set_zone_debounce_ms(uint32_t ms) { zone_debounce_ms_ = ms; }
  void set_zone_code_override(uint8_t zone, uint16_t code) {
    if (zone > 0 && zone <= MAX_ZONES) zone_code_overrides_[zone] = code;
  }

  void add_virtual_zone(uint8_t zone, uint16_t packet_code,
                         bool clear_on_close, bool trigger_on_open,
                         binary_sensor::BinarySensor *sensor);

  // ── Entity Registration ──
  void set_alarm_control_panel(alarm_control_panel::AlarmControlPanel *acp) { acp_ = acp; }
  void set_estoy_sensor(binary_sensor::BinarySensor *s) { estoy_sensor_ = s; }
  void set_zone_sensor(uint8_t zone, binary_sensor::BinarySensor *s);
  void set_sniffer_text_sensor(text_sensor::TextSensor *s) { sniffer_text_ = s; }

  // ── Services ──
  void send_keys_service(const std::string &keys);
  void send_packet(uint16_t code) { bus_.send_packet(code); }
  void enter_programming_service();
  void enter_advanced_service();
  void exit_programming_service();
  void set_entry_delay_service(int seconds);
  void set_exit_delay_service(int seconds);
  void set_siren_duration_service(int minutes);
  void set_siren_b_duration_service(int minutes);
  void set_sabotage_inhibit_service(bool enabled);
  void set_ac_frequency_service(int hz);
  void set_entry_annunciator_service(bool enabled);
  void set_battery_save_service(bool enabled);
  void set_owner_code_condition_service(bool disarm_only);
  void set_zone_conditionality_service(bool enabled);
  void change_owner_code_service(const std::string &new_code);
  void change_installer_code_service(const std::string &new_code);
  void program_user_service(int user, const std::string &code, int permissions, bool can_disarm);
  void rf_learn_mode_service();
  void rf_learn_slot_service(int slot);
  void rf_delete_slot_service(int slot);
  void exit_rf_learning_service();
  void toggle_zone_in_mode_service(int zone);
  void save_estoy_config_service();
  void save_mevoy_config_service();
  void set_zone_type_service(int zone, const std::string &type);
  void set_panic_zone_service();
  void set_tamper_zone_service();
  void set_clock_source_service(bool crystal);
  void set_wired_zones_service(bool enabled);
  void set_partition_merge_service(bool enabled);
  void set_pgm_output_service(int output, int option, int partition);

  // ── HA Alarm Control Panel Actions ──
  void arm_away();
  void arm_home();
  void disarm();

  // ── Event Handlers ──
  void on_event(uint16_t word);
  void on_packet(uint16_t word);
  void on_zone(uint8_t zone, bool triggered);

 protected:
  struct VirtualZoneState {
    uint8_t zone;
    uint16_t packet_code;
    bool clear_on_close;
    binary_sensor::BinarySensor *sensor;
    bool trigger_on_open;
    bool last_state;
  };

  void send_programmed_sequence(const std::string &body, bool use_installer, bool advanced, bool exit_f = true);
  void toggle_mode(uint16_t target_mode);

  MPXBus bus_;
  InternalGPIOPin *rx_pin_{nullptr};
  InternalGPIOPin *tx_pin_{nullptr};
  bool invert_rx_{true};
  bool invert_tx_{true};

  std::string code_;
  std::string installer_code_ = "467825";
  X28Model model_{X28Model::AUTO};
  ModelCapabilities model_capabilities_{get_model_capabilities(X28Model::AUTO)};
  bool debug_{false};
  bool sniffing_enabled_{false};
  uint32_t sniffing_throttle_ms_{1000};
  uint32_t zone_debounce_ms_{500};

  uint16_t last_mode_{MPX_CODE_ME_VOY};
  bool armed_confirmed_{false};
  uint32_t arm_pending_start_{0};
  bool arm_pending_{false};
  bool disarm_pending_{false};
  bool mode_waiting_{false};
  uint32_t last_sniff_log_{0};
  uint16_t last_sniff_word_{0};
  uint32_t zone_last_packet_[MAX_ZONES]{};

  alarm_control_panel::AlarmControlPanel *acp_{nullptr};
  binary_sensor::BinarySensor *estoy_sensor_{nullptr};
  binary_sensor::BinarySensor *zone_sensors_[MAX_ZONES]{};
  text_sensor::TextSensor *sniffer_text_{nullptr};

  std::vector<VirtualZoneState> virtual_zones_;
  uint16_t zone_code_overrides_[MAX_ZONES + 1]{};
};

// ─── X28AlarmControlPanel ────────────────────────────────────────────────
class X28AlarmControlPanel : public alarm_control_panel::AlarmControlPanel {
 public:
  void set_parent(X28Alarm *parent) { parent_ = parent; }

 protected:
  bool arm_away() override;
  bool arm_home() override;
  bool disarm() override;
  bool arm_night() override { return false; }

 private:
  X28Alarm *parent_{nullptr};
};

// ─── X28ActionButton ─────────────────────────────────────────────────────
class X28ActionButton : public button::Button {
 public:
  void set_parent(X28Alarm *parent) { parent_ = parent; }
  void set_code(uint16_t code) { code_ = code; }
  void set_long_code(uint16_t code) { long_code_ = code; }

 protected:
  void press_action() override;

 private:
  X28Alarm *parent_{nullptr};
  uint16_t code_{0};
  uint16_t long_code_{0};
};

}  // namespace x28_alarm
}  // namespace esphome
