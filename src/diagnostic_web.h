#pragma once

#include <Arduino.h>
#include <atomic>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct DiagnosticRuntimeState {
  bool test_mode = false;
  bool can_started = false;
  bool can_locked = false;
  bool can_bitrate_saved = false;
  bool mdi_detected = false;
  bool mapping_verified = false;
  bool dm1_available = false;
  bool alarm_data_fresh = false;
  bool oil_alarm_valid = false;
  bool temperature_alarm_valid = false;
  bool voltage_alarm_valid = false;
  bool over_temperature = false;
  bool low_oil = false;
  bool low_voltage = false;
  bool engine_check = false;
  // Experimental rules copied literally from the 2007 Volvo MDI workshop
  // manual. They never drive the verified Signal K alarm outputs above.
  bool rule_oil_valid = false;
  bool rule_oil_condition = false;
  bool rule_oil_on = false;
  bool rule_temp_valid = false;
  bool rule_temp_condition = false;
  bool rule_temp_on = false;
  bool rule_low_voltage_valid = false;
  bool rule_low_voltage_condition = false;
  bool rule_low_voltage_on = false;
  bool rule_high_voltage_valid = false;
  bool rule_high_voltage_condition = false;
  bool rule_high_voltage_on = false;
  uint32_t rule_oil_elapsed_ms = 0;
  uint32_t rule_oil_threshold_ms = 0;
  uint32_t rule_oil_changed_ms = 0;
  uint32_t rule_temp_elapsed_ms = 0;
  uint32_t rule_temp_changed_ms = 0;
  uint32_t rule_low_voltage_elapsed_ms = 0;
  uint32_t rule_low_voltage_changed_ms = 0;
  uint32_t rule_high_voltage_elapsed_ms = 0;
  uint32_t rule_high_voltage_changed_ms = 0;
  uint16_t can_bitrate_kbps = 0;
  uint8_t engine_source = 0xff;
  uint32_t can_raw_frame_count = 0;
  uint32_t can_extended_frame_count = 0;
  uint32_t can_standard_frame_count = 0;
  uint32_t frame_count = 0;
  uint32_t unknown_frame_count = 0;
  uint32_t latest_pgn = 0;
  float rpm = NAN;
  float coolant_c = NAN;
  float alternator_v = NAN;
  float battery_v = NAN;
  float mdi_supply_v = NAN;
  float oil_kpa = NAN;
  char signalk_status[32] = "Inicializando";
  char signalk_server[64] = "Sin configurar";
  uint16_t signalk_port = 0;
};

class DiagnosticWeb {
 public:
  void begin(uint16_t port = 8080, const char* firmware_password = nullptr);
  // Invoked when the operator asks the panel to forget the learned CAN
  // bitrate. Runs on the web task; the callback must be safe to call there.
  void set_can_bitrate_reset_handler(bool (*handler)());
  void set_runtime_state(const DiagnosticRuntimeState& state);
  void note_j1939_frame(uint32_t can_id, uint32_t pgn, uint8_t source,
                         const uint8_t* data, uint8_t len);

 private:
  static constexpr uint8_t kFrameSlots = 32;
  static constexpr uint8_t kPersistentSlots = 64;
  static constexpr uint16_t kCaptureSlots = 160;
  static constexpr uint16_t kPersistentQueueSlots = 256;
  static constexpr uint8_t kResearchQueueSlots = 24;
  static constexpr uint8_t kResearchHistorySlots = 16;

  struct FrameSlot {
    bool used = false;
    uint32_t can_id = 0;
    uint32_t pgn = 0;
    uint32_t count = 0;
    uint32_t first_ms = 0;
    uint32_t last_ms = 0;
    uint32_t last_capture_ms = 0;
    uint8_t source = 0xff;
    uint8_t len = 0;
    uint8_t data[8] = {};
    uint8_t captured_data[8] = {};
  };

  struct CaptureRow {
    uint32_t timestamp_ms = 0;
    uint32_t relative_ms = 0;
    uint32_t can_id = 0;
    uint32_t pgn = 0;
    uint16_t session = 0;
    uint8_t kind = 0;  // 0 = CAN frame, 1 = observed panel marker
    uint8_t marker_id = 0;
    bool marker_active = false;
    uint8_t source = 0xff;
    uint8_t len = 0;
    uint8_t data[8] = {};
  };

  struct PersistentRow {
    uint32_t uptime_ms = 0;
    uint32_t can_id = 0;
    uint32_t pgn = 0;
    uint8_t source = 0xff;
    uint8_t len = 0;
    uint8_t raw_changed_mask = 0;
    uint8_t trigger_mask = 0;
    bool baseline = false;
    bool context = false;
    uint16_t session = 0;
    uint8_t data[8] = {};
    uint8_t xor_data[8] = {};
  };

  struct PersistentSlot {
    bool used = false;
    uint32_t pgn = 0;
    uint32_t last_ms = 0;
    uint8_t source = 0xff;
    // Proprietary Volvo PGNs 65417/65420 multiplex several records in the
    // same PGN. Keeping one baseline per subtype prevents consecutive,
    // unrelated records from looking like a change of every byte.
    uint8_t subtype = 0;
    uint8_t len = 0;
    uint8_t data[8] = {};
    // Last accepted row: used only for telemetry sampling, never to suppress
    // a second bit transition. ABA changes must both survive.
    uint32_t last_row_ms = 0;
    uint8_t last_trigger_mask = 0;
  };

  struct ResearchRow {
    uint32_t uptime_ms = 0;
    uint32_t hypothesis_id = 0;
    uint32_t pgn = 0;
    uint8_t event = 0;  // 0 = hypothesis, 1 = confirmation, 2 = observation
    uint8_t code = 0;
    uint8_t response = 0;  // 0 = pending/not applicable, 1 = yes, 2 = no
    uint8_t len = 0;
    uint8_t data[8] = {};
  };

  struct ResearchItem {
    uint32_t hypothesis_id = 0;
    uint32_t uptime_ms = 0;
    uint32_t pgn = 0;
    uint8_t code = 0;
    uint8_t response = 0;
    uint8_t len = 0;
    uint8_t data[8] = {};
  };

  static void task_entry(void* parameter);
  void task_loop();
  void serve_client(WiFiClient& client);
  void send_page(WiFiClient& client);
  void send_research_page(WiFiClient& client);
  void send_research_status(WiFiClient& client);
  void send_research_capture(WiFiClient& client);
  void send_status(WiFiClient& client);
  void send_frames(WiFiClient& client);
  void send_capture(WiFiClient& client);
  void send_persistent_capture(WiFiClient& client, bool previous = false);
  bool receive_firmware(WiFiClient& client, size_t content_length);
  void send_session(WiFiClient& client);
  void send_json_ok(WiFiClient& client);
  void send_json_result(WiFiClient& client, bool ok, const char* error = nullptr);
  void send_forbidden(WiFiClient& client);
  void send_method_not_allowed(WiFiClient& client);
  void send_not_found(WiFiClient& client);
  bool set_capture(bool enabled, bool clear);
  bool set_marker(uint8_t marker_id, bool active);
  void append_capture_row_locked(const CaptureRow& row);
  bool track_persistent_change(uint32_t can_id, uint32_t pgn, uint8_t source,
                               const uint8_t* data, uint8_t len,
                               uint32_t now);
  void begin_persistent_capture();
  void drain_persistent_queue();
  void append_persistent_row(const PersistentRow& row);
  void rotate_persistent_capture_if_needed();
  bool persistent_space_available();
  bool clear_persistent_capture();
  void detect_research_hypothesis(uint32_t pgn, uint8_t source,
                                  const uint8_t* data, uint8_t len,
                                  uint32_t now);
  void publish_research_hypothesis(uint8_t code, uint32_t pgn,
                                   const uint8_t* data, uint8_t len,
                                   uint32_t now);
  bool respond_to_research_hypothesis(uint32_t hypothesis_id,
                                      bool confirmed);
  // Queues a human observation without touching SPIFFS in the HTTP request
  // path. event_uptime_ms may be supplied by the browser after synchronizing
  // performance.now() with the ESP32 uptime clock.
  bool add_research_observation(uint8_t code, uint32_t event_uptime_ms = 0);
  void drain_research_queue();
  void append_research_row(const ResearchRow& row);

  bool (*can_bitrate_reset_handler_)() = nullptr;
  // Mirror of the selected engine source address, published by the CAN task
  // through set_runtime_state(). A single byte, so it is read without the
  // mutex from the J1939 decode path.
  volatile uint8_t persistent_engine_source_ = 0xff;
  std::atomic<uint32_t> baseline_epoch_{0};
  uint32_t applied_baseline_epoch_ = 0;
  std::atomic<uint16_t> recording_session_{0};
  uint32_t last_engine_frame_ms_ = 0;
  uint32_t context_started_ms_ = 0;
  bool context_active_ = false;

  WiFiServer* server_ = nullptr;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  QueueHandle_t persistent_queue_ = nullptr;
  QueueHandle_t research_queue_ = nullptr;
  DiagnosticRuntimeState runtime_;
  FrameSlot frames_[kFrameSlots];
  PersistentSlot persistent_slots_[kPersistentSlots];
  CaptureRow capture_[kCaptureSlots];
  uint16_t capture_head_ = 0;
  uint16_t capture_count_ = 0;
  bool capture_enabled_ = false;
  uint32_t capture_started_ms_ = 0;
  uint16_t capture_session_ = 0;
  bool marker_states_[7] = {};
  char csrf_token_[17] = {};
  char firmware_password_[24] = {};
  uint32_t persistent_boot_id_ = 0;
  uint32_t persistent_rows_ = 0;
  volatile uint32_t persistent_dropped_ = 0;
  size_t persistent_bytes_ = 0;
  bool persistent_ready_ = false;
  // Set when the filesystem can no longer take capture rows. The SensESP
  // WiFi/Signal K configuration must always keep room to be rewritten, so the
  // capture yields the remaining space instead of consuming it.
  bool persistent_full_ = false;
  bool persistent_write_error_ = false;
  const char* persistent_error_reason_ = "";
  uint32_t persistent_write_errors_ = 0;
  uint32_t persistent_verify_errors_ = 0;
  bool session_write_error_ = false;
  bool session_full_ = false;
  size_t session_bytes_ = 0;
  uint32_t session_rows_ = 0;
  uint32_t session_dropped_ = 0;
  uint32_t capture_overwritten_ = 0;
  bool research_write_error_ = false;
  uint32_t research_dropped_ = 0;
  uint32_t research_next_hypothesis_id_ = 0;
  uint32_t research_hypothesis_id_ = 0;
  uint32_t research_hypothesis_ms_ = 0;
  uint32_t research_hypothesis_pgn_ = 0;
  uint8_t research_hypothesis_code_ = 0;
  uint8_t research_hypothesis_response_ = 0;
  uint8_t research_hypothesis_len_ = 0;
  uint8_t research_hypothesis_data_[8] = {};
  ResearchItem research_history_[kResearchHistorySlots];
  uint8_t research_history_head_ = 0;
  uint8_t research_history_count_ = 0;
  bool research_mdi_seen_ = false;
  bool research_a0_seen_ = false;
  bool research_a0_beep_active_ = false;
  bool research_b2_seen_ = false;
  bool research_b2_crank_active_ = false;
  bool research_c1_seen_ = false;
  bool research_c1_low_voltage_active_ = false;
  uint32_t research_rows_ = 0;
  size_t research_bytes_ = 0;
  uint16_t port_ = 8080;
};
