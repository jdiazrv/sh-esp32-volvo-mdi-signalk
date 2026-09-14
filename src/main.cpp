#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DallasTemperature.h>
#include <OneWire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/twai.h>
#include <esp_random.h>

#include <ArduinoJson.h>
#include <SPIFFS.h>

#include "sensesp.h"
#include "sensesp_app_builder.h"
#include "sensesp/signalk/signalk_output.h"
#include "sensesp/system/hash.h"
#include "diagnostic_web.h"

using namespace sensesp;

static constexpr gpio_num_t kCanTxPin = GPIO_NUM_32;
static constexpr gpio_num_t kCanRxPin = GPIO_NUM_34;
static constexpr uint8_t kOneWirePin = 4;
static constexpr uint8_t kI2cSdaPin = 16;
static constexpr uint8_t kI2cSclPin = 17;
static constexpr uint8_t kScreenWidth = 128;
static constexpr uint8_t kScreenHeight = 64;

static constexpr uint32_t kPgnEec1 = 61444;       // Engine speed
static constexpr uint32_t kPgnTpDt = 60160;       // J1939 transport protocol data
static constexpr uint32_t kPgnTpCm = 60416;       // J1939 transport protocol control
static constexpr uint32_t kPgnHours = 65253;      // Total engine hours
static constexpr uint32_t kPgnTemp1 = 65262;      // Engine temperature 1
static constexpr uint32_t kPgnFluidPressure = 65263;
static constexpr uint32_t kPgnElectrical = 65271; // Vehicle electrical power
static constexpr uint32_t kPgnDashDisplay = 65276; // Fuel level
static constexpr uint32_t kPgnDm1 = 65226;        // Active diagnostic trouble codes
static constexpr uint32_t kPgnAirIntake = 65264;
static constexpr uint32_t kPgnShutdown = 65252;   // Shutdown / alarm status
static constexpr uint32_t kPgnVolvoWarnings = 65417;
static constexpr uint32_t kPgnTestUnknown = 65352;
static constexpr uint8_t kMaxUnknownPgns = 24;
static constexpr uint32_t kDm1TimeoutMs = 5000;
static constexpr uint32_t kAlarmHeartbeatMs = 3000;
static constexpr uint32_t kFastTelemetryPublishIntervalMs = 500;
static constexpr uint32_t kSlowTelemetryPublishIntervalMs = 2000;
static constexpr uint32_t kTestScenarioMs = 5000;
static constexpr uint32_t kTpTimeoutMs = 1500;
static constexpr uint32_t kCanProbeWindowMs = 3000;
// Long enough that a normal engine stop or a brief bus dropout never releases
// the source, short enough to recover from a wrong pick without a reboot.
static constexpr uint32_t kEngineSourceTimeoutMs = 60000;
static constexpr uint16_t kTpBufferSize = 512;
static constexpr char kCanPreferencesNamespace[] = "volvo-mdi";
static constexpr char kCanBitratePreferenceKey[] = "can-kbps";
static constexpr char kApPasswordPreferenceKey[] = "ap-pass";
static constexpr uint8_t kGeneratedApPasswordLength = 12;
static constexpr uint8_t kMinApPasswordLength = 8;

#ifndef VOLVO_MDI_TEST_MODE
#define VOLVO_MDI_TEST_MODE 0
#endif

#ifndef VOLVO_MDI_ENGINE_SOURCE
#define VOLVO_MDI_ENGINE_SOURCE 0xff
#endif

#ifndef VOLVO_MDI_TRACE_ALL
#define VOLVO_MDI_TRACE_ALL VOLVO_MDI_TEST_MODE
#endif

// Volvo has not published the D1/D2 MDI PGN 65417 bit layout. A production
// build therefore decodes only masks explicitly supplied by the installer;
// raw bytes remain only in the local diagnostic page/capture for verification.
// TEST uses a documented synthetic layout, never silently reused by LIVE.
#if VOLVO_MDI_TEST_MODE
#define MDI_MASK_OVER_TEMP (1ULL << 0)
#define MDI_MASK_LOW_OIL (1ULL << 1)
#define MDI_MASK_LOW_VOLTAGE (1ULL << 2)
#define MDI_MASK_PREHEAT (1ULL << 3)
#define MDI_MASK_STARTING (1ULL << 4)
#define MDI_MASK_STOPPING (1ULL << 5)
#define MDI_MASK_SYSTEM_FAULT (1ULL << 6)
#define MDI_MASK_AUXILIARY_FAULT (1ULL << 7)
#else
#ifndef MDI_MASK_OVER_TEMP
#define MDI_MASK_OVER_TEMP 0ULL
#endif
#ifndef MDI_MASK_LOW_OIL
#define MDI_MASK_LOW_OIL 0ULL
#endif
#ifndef MDI_MASK_LOW_VOLTAGE
#define MDI_MASK_LOW_VOLTAGE 0ULL
#endif
#ifndef MDI_MASK_PREHEAT
#define MDI_MASK_PREHEAT 0ULL
#endif
#ifndef MDI_MASK_STARTING
#define MDI_MASK_STARTING 0ULL
#endif
#ifndef MDI_MASK_STOPPING
#define MDI_MASK_STOPPING 0ULL
#endif
#ifndef MDI_MASK_SYSTEM_FAULT
#define MDI_MASK_SYSTEM_FAULT 0ULL
#endif
#ifndef MDI_MASK_AUXILIARY_FAULT
#define MDI_MASK_AUXILIARY_FAULT 0ULL
#endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD ""
#endif

#ifndef SIGNALK_HOST
#define SIGNALK_HOST ""
#endif

#ifndef SIGNALK_PORT
#define SIGNALK_PORT 3000
#endif

OneWire one_wire(kOneWirePin);
DallasTemperature dallas(&one_wire);
TwoWire display_i2c = TwoWire(0);
Adafruit_SSD1306 display(kScreenWidth, kScreenHeight, &display_i2c, -1);
bool display_ok = false;
char ota_password[24] = {};

SKOutputFloat* sk_rpm = nullptr;
SKOutputFloat* sk_hours = nullptr;
SKOutputFloat* sk_coolant_temp = nullptr;
SKOutputFloat* sk_alternator_voltage = nullptr;
SKOutputFloat* sk_start_battery_voltage = nullptr;
SKOutputFloat* sk_mdi_supply_voltage = nullptr;
SKOutputFloat* sk_oil_pressure = nullptr;
SKOutputFloat* sk_fuel_level = nullptr;
SKOutputBool* sk_over_temperature_alarm = nullptr;
SKOutputBool* sk_low_oil_pressure_alarm = nullptr;
SKOutputBool* sk_low_voltage_alarm = nullptr;
SKOutputBool* sk_glow_plug_fault_alarm = nullptr;
SKOutputBool* sk_engine_check_alarm = nullptr;
SKOutputFloat* sk_active_dtc_count = nullptr;
SKOutputFloat* sk_first_dtc_spn = nullptr;
SKOutputFloat* sk_first_dtc_fmi = nullptr;

SKOutputFloat* sk_engine_room_temp = nullptr;
SKOutputFloat* sk_exhaust_temp = nullptr;
SKOutputFloat* sk_aux_temp = nullptr;

float latest_rpm = NAN;
float latest_coolant_c = NAN;
float latest_alternator_v = NAN;
float latest_battery_v = NAN;
float latest_mdi_supply_v = NAN;
float latest_oil_kpa = NAN;
uint32_t latest_pgn = 0;
uint32_t frame_count = 0;
uint32_t latest_unknown_pgn = 0;
uint32_t unknown_frame_count = 0;
uint32_t last_dm1_ms = 0;
uint32_t last_mdi_warning_ms = 0;
uint32_t last_eec1_publish_ms = 0;
bool eec1_published = false;
uint32_t last_hours_publish_ms = 0;
uint32_t last_temp_publish_ms = 0;
uint32_t last_electrical_publish_ms = 0;
uint32_t last_oil_publish_ms = 0;
uint32_t last_rpm_rx_ms = 0;
uint32_t last_temp_rx_ms = 0;
uint32_t last_mdi_supply_rx_ms = 0;
uint32_t last_oil_rx_ms = 0;
bool hours_published = false;
bool temp_published = false;
bool electrical_published = false;
bool oil_published = false;
bool over_temperature_alarm = false;
bool low_oil_pressure_alarm = false;
bool low_voltage_alarm = false;
bool glow_plug_fault_alarm = false;
bool preheat_active = false;
bool engine_check_alarm = false;
bool mdi_detected = false;
// Repeated captures from this D1/D2 identify C1 byte 3 bit 0x20 as the MDI's
// low-system-voltage warning. Its rising edge follows SPN 158 <= 13 V by
// about ten seconds while running and it clears when charging recovers.
constexpr bool kMdiC1LowVoltageVerified = !VOLVO_MDI_TEST_MODE;
constexpr bool kMdiMaskOverTemperatureVerified = MDI_MASK_OVER_TEMP != 0;
constexpr bool kMdiMaskLowOilVerified = MDI_MASK_LOW_OIL != 0;
constexpr bool kMdiMaskLowVoltageVerified = MDI_MASK_LOW_VOLTAGE != 0;
constexpr bool kMdiMaskSystemFaultVerified = MDI_MASK_SYSTEM_FAULT != 0;
constexpr bool kMdiMaskAuxiliaryFaultVerified = MDI_MASK_AUXILIARY_FAULT != 0;
bool mdi_mapping_verified =
    kMdiC1LowVoltageVerified || kMdiMaskOverTemperatureVerified ||
    kMdiMaskLowOilVerified || kMdiMaskLowVoltageVerified ||
    MDI_MASK_PREHEAT != 0 || MDI_MASK_STARTING != 0 ||
    MDI_MASK_STOPPING != 0 || kMdiMaskSystemFaultVerified ||
    kMdiMaskAuxiliaryFaultVerified;
bool dm1_available = false;
bool engine_starting = false;
bool engine_stopping = false;
bool system_fault = false;
bool auxiliary_fault = false;
uint8_t selected_engine_source = VOLVO_MDI_ENGINE_SOURCE;
uint32_t last_engine_source_ms = 0;
bool dm1_over_temperature = false;
bool dm1_low_oil = false;
bool dm1_low_voltage = false;
bool dm1_glow_plug = false;
bool mdi_over_temperature = false;
bool mdi_low_oil = false;
bool mdi_low_voltage = false;
bool dm1_check_engine = false;

constexpr uint32_t kRuleSignalTimeoutMs = 5000;
constexpr float kRuleOilThresholdKpa = 60.0f;
constexpr float kRuleRunningRpm = 50.0f;
constexpr float kRuleLowVoltage = 13.0f;
constexpr float kRuleHighVoltage = 15.0f;
constexpr float kRuleHighTemperatureC = 110.0f;

struct TimedManualRule {
  bool valid = false;
  bool condition = false;
  bool on = false;
  bool timing = false;
  uint32_t started_ms = 0;
  uint32_t elapsed_ms = 0;
  uint32_t threshold_ms = 0;
  uint32_t changed_ms = 0;
};

TimedManualRule manual_oil_rule;
TimedManualRule manual_temp_rule;
TimedManualRule manual_low_voltage_rule;
TimedManualRule manual_high_voltage_rule;

struct TransportSession {
  bool active = false;
  uint8_t source = 0xff;
  uint8_t next_sequence = 1;
  uint8_t total_packets = 0;
  uint16_t total_size = 0;
  uint16_t received = 0;
  uint32_t target_pgn = 0;
  uint32_t started_ms = 0;
  uint8_t payload[kTpBufferSize] = {};
};

TransportSession tp_session;
bool can_started = false;
bool can_bitrate_locked = false;
uint16_t current_can_bitrate_kbps = 250;
uint16_t saved_can_bitrate_kbps = 0;
uint32_t can_probe_started_ms = 0;
uint32_t raw_can_frame_count = 0;
uint32_t extended_can_frame_count = 0;
uint32_t standard_can_frame_count = 0;
DiagnosticWeb diagnostic_web;

struct UnknownPgnCounter {
  uint32_t pgn = 0;
  uint32_t count = 0;
};

UnknownPgnCounter unknown_pgns[kMaxUnknownPgns];

uint32_t extract_pgn(uint32_t can_id) {
  uint8_t pf = (can_id >> 16) & 0xff;

  if (pf < 240) {
    return (can_id >> 8) & 0x3ff00;
  }
  return (can_id >> 8) & 0x3ffff;
}

uint8_t extract_source(uint32_t can_id) {
  return can_id & 0xff;
}

bool valid_u8(uint8_t value) {
  return value < 0xfb;
}

bool valid_u16(uint16_t value) {
  return value < 0xfb00;
}

uint16_t le_u16(const uint8_t* data, uint8_t offset) {
  return uint16_t(data[offset]) | (uint16_t(data[offset + 1]) << 8);
}

uint32_t le_u32(const uint8_t* data, uint8_t offset) {
  return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
         (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
}

uint64_t le_u64(const uint8_t* data) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < 8; i++) {
    value |= uint64_t(data[i]) << (8 * i);
  }
  return value;
}

// Auto-selection must be driven by a standard engine-data frame (EEC1), not
// by a Volvo proprietary warning frame.  On a real MultiLink bus PGN 65417 is
// also transmitted by the tachometer/display (observed at SA 0xF2), while the
// MDI's EEC1, hours, temperature and electrical PGNs come from SA 0x00.  Using
// the first proprietary frame used to latch 0xF2 and silently discard every
// standard engine PGN from 0x00, so RPM and runTime never reached Signal K.
bool consider_engine_source(uint8_t source) {
  if (selected_engine_source == 0xff) {
    selected_engine_source = source;
    Serial.printf("Engine source selected: SA 0x%02x\n", source);
  }
  const bool match = source == selected_engine_source;
  if (match) last_engine_source_ms = millis();
  return match;
}

// A source latched from a spurious EEC1 used to be permanent: every engine PGN
// from the real ECU was then discarded until the next reboot, with no way to
// recover from the panel. Release the selection when the chosen address has
// been silent long enough that it cannot be the engine that is running.
void check_engine_source_timeout() {
  if (selected_engine_source == 0xff || last_engine_source_ms == 0) return;
  if (VOLVO_MDI_ENGINE_SOURCE != 0xff) return;  // Pinned at build time.
  if (millis() - last_engine_source_ms < kEngineSourceTimeoutMs) return;
  Serial.printf(
      "Engine source SA 0x%02x silent for %lu s; releasing the selection\n",
      selected_engine_source, (unsigned long)(kEngineSourceTimeoutMs / 1000));
  selected_engine_source = 0xff;
  last_engine_source_ms = 0;
}

uint32_t make_j1939_id(uint8_t priority, uint32_t pgn, uint8_t source) {
  uint8_t pf = (pgn >> 8) & 0xff;
  uint8_t ps = pgn & 0xff;

  if (pf < 240) {
    ps = 0xff;  // Global destination for PDU1 test frames.
  }

  return (uint32_t(priority & 0x7) << 26) | ((pgn & 0x3ff00) << 8) |
         (uint32_t(ps) << 8) | source;
}

twai_message_t make_test_frame(uint32_t pgn, const uint8_t* data, uint8_t len) {
  twai_message_t msg = {};
  msg.identifier = make_j1939_id(3, pgn, 0x80);
  msg.flags = TWAI_MSG_FLAG_EXTD;
  msg.extd = 1;
  msg.data_length_code = len;
  memcpy(msg.data, data, len);
  return msg;
}

void print_frame(uint32_t id, uint32_t pgn, const uint8_t* data, uint8_t len) {
  Serial.printf("J1939 id=0x%08lx pgn=%lu sa=0x%02x data=", id, pgn, extract_source(id));
  for (uint8_t i = 0; i < len; i++) {
    Serial.printf("%02x", data[i]);
    if (i + 1 < len) {
      Serial.print(' ');
    }
  }
  Serial.println();
}

void parse_eec1(const uint8_t* data, uint8_t len) {
  if (len < 5) {
    return;
  }
  uint16_t raw_rpm = le_u16(data, 3);
  if (!valid_u16(raw_rpm)) {
    return;
  }

  float rpm = raw_rpm * 0.125f;
  latest_rpm = rpm;
  const uint32_t now = millis();
  last_rpm_rx_ms = now;
  const bool publish = !eec1_published ||
                       now - last_eec1_publish_ms >=
                           kFastTelemetryPublishIntervalMs;
  if (!publish) return;
  eec1_published = true;
  last_eec1_publish_ms = now;

  if (sk_rpm != nullptr) {
    sk_rpm->set(rpm / 60.0f);  // Signal K uses Hz (revolutions per second).
  }

}

void parse_hours(const uint8_t* data, uint8_t len) {
  if (len < 4) {
    return;
  }

  uint32_t raw_hours = le_u32(data, 0);
  if (raw_hours >= 0xfb000000UL) {
    return;
  }

  float hours = raw_hours * 0.05f;
  const uint32_t now = millis();
  if (hours_published &&
      now - last_hours_publish_ms < kSlowTelemetryPublishIntervalMs) {
    return;
  }
  hours_published = true;
  last_hours_publish_ms = now;
  if (sk_hours != nullptr) {
    sk_hours->set(hours * 3600.0f);  // Signal K duration is seconds.
  }
}

void parse_temp1(const uint8_t* data, uint8_t len) {
  if (len < 1 || !valid_u8(data[0])) {
    return;
  }

  float coolant_kelvin = float(data[0]) - 40.0f + 273.15f;
  latest_coolant_c = coolant_kelvin - 273.15f;
  const uint32_t now = millis();
  last_temp_rx_ms = now;
  if (temp_published &&
      now - last_temp_publish_ms < kSlowTelemetryPublishIntervalMs) {
    return;
  }
  temp_published = true;
  last_temp_publish_ms = now;
  if (sk_coolant_temp != nullptr) {
    sk_coolant_temp->set(coolant_kelvin);
  }
}

void parse_fluid_pressure(const uint8_t* data, uint8_t len) {
  if (len < 4 || !valid_u8(data[3])) {
    return;
  }

  float oil_pressure_pa = float(data[3]) * 4.0f * 1000.0f;
  latest_oil_kpa = oil_pressure_pa / 1000.0f;
  const uint32_t now = millis();
  last_oil_rx_ms = now;
  if (oil_published &&
      now - last_oil_publish_ms < kSlowTelemetryPublishIntervalMs) {
    return;
  }
  oil_published = true;
  last_oil_publish_ms = now;
  if (sk_oil_pressure != nullptr) {
    sk_oil_pressure->set(oil_pressure_pa);
  }
}

void parse_electrical(const uint8_t* data, uint8_t len) {
  if (len < 8) {
    return;
  }

  const uint32_t now = millis();

  // J1939 VEP1: SPN 167 charging potential is bytes 3-4, SPN 168 battery
  // input is 5-6, and SPN 158 keyswitch/MDI supply is 7-8. D1/D2 commonly
  // publish only the last one; do not mislabel it as an alternator value.
  const uint16_t raw_charging = le_u16(data, 2);
  if (valid_u16(raw_charging)) {
    latest_alternator_v = raw_charging * 0.05f;
  }

  const uint16_t raw_battery = le_u16(data, 4);
  const bool has_battery_potential = valid_u16(raw_battery);
  if (has_battery_potential) {
    latest_battery_v = raw_battery * 0.05f;
  }

  const uint16_t raw_keyswitch = le_u16(data, 6);
  if (valid_u16(raw_keyswitch)) {
    const float supply_v = raw_keyswitch * 0.05f;
    latest_mdi_supply_v = supply_v;
    last_mdi_supply_rx_ms = now;
    // SPN 158 is the keyswitch/MDI supply. It often follows the starter
    // battery closely, but it is not SPN 168 and must never be exported as
    // electrical.batteries.start.voltage. Keeping the two meanings separate
    // prevents consumers from inventing a starter-battery sensor when this
    // MDI only provides its own supply voltage.
  }
  if (electrical_published &&
      now - last_electrical_publish_ms < kSlowTelemetryPublishIntervalMs) {
    return;
  }
  electrical_published = true;
  last_electrical_publish_ms = now;
  if (valid_u16(raw_charging) && sk_alternator_voltage != nullptr)
    sk_alternator_voltage->set(latest_alternator_v);
  if (has_battery_potential && sk_start_battery_voltage != nullptr)
    sk_start_battery_voltage->set(latest_battery_v);
  if (valid_u16(raw_keyswitch) && sk_mdi_supply_voltage != nullptr)
    sk_mdi_supply_voltage->set(latest_mdi_supply_v);
}

void note_alarm_frame(uint32_t pgn, const uint8_t* data, uint8_t len) {
  Serial.printf("ALARM/DIAG candidate pgn=%lu data=", pgn);
  for (uint8_t i = 0; i < len; i++) {
    Serial.printf("%02x", data[i]);
    if (i + 1 < len) {
      Serial.print(' ');
    }
  }
  Serial.println();
}

void publish_alarm_states() {
  static uint32_t last_publish_ms = 0;
  static bool reported[5] = {};
  static bool last_state[5] = {};

  const uint32_t now = millis();
  const bool dm1_fresh =
      last_dm1_ms != 0 && now - last_dm1_ms <= kDm1TimeoutMs;
  const bool mdi_fresh = mdi_mapping_verified && last_mdi_warning_ms != 0 &&
                         now - last_mdi_warning_ms <= kDm1TimeoutMs;
  const bool heartbeat = now - last_publish_ms >= kAlarmHeartbeatMs;
  auto publish_if_relevant = [heartbeat](SKOutputBool* output, bool valid,
                                         bool active, bool& was_reported,
                                         bool& previous_state) {
    // A timeout means UNKNOWN, not NORMAL. Stop refreshing the last boolean so
    // consumers can expire it by timestamp; only a fresh source may clear it.
    if (output == nullptr || !valid) return;
    if (!was_reported || active != previous_state || heartbeat) {
      output->set(active);
    }
    was_reported = true;
    previous_state = active;
  };

  const bool mdi_over_temperature_fresh =
      mdi_fresh && kMdiMaskOverTemperatureVerified;
  const bool mdi_low_oil_fresh = mdi_fresh && kMdiMaskLowOilVerified;
  const bool mdi_low_voltage_fresh =
      mdi_fresh &&
      (kMdiC1LowVoltageVerified || kMdiMaskLowVoltageVerified);
  const bool mdi_engine_check_fresh =
      mdi_fresh &&
      (kMdiMaskSystemFaultVerified || kMdiMaskAuxiliaryFaultVerified);
  const bool verified_over_temperature =
      (dm1_fresh && dm1_over_temperature) ||
      (mdi_over_temperature_fresh && mdi_over_temperature);
  const bool verified_low_oil =
      (dm1_fresh && dm1_low_oil) || (mdi_low_oil_fresh && mdi_low_oil);
  const bool verified_low_voltage =
      (dm1_fresh && dm1_low_voltage) ||
      (mdi_low_voltage_fresh && mdi_low_voltage);
  const bool verified_glow_plug = dm1_fresh && dm1_glow_plug;
  const bool verified_engine_check =
      (dm1_fresh && dm1_check_engine) ||
      (mdi_engine_check_fresh && (system_fault || auxiliary_fault));

  publish_if_relevant(sk_over_temperature_alarm,
                      dm1_fresh || mdi_over_temperature_fresh,
                      verified_over_temperature,
                      reported[0], last_state[0]);
  publish_if_relevant(sk_low_oil_pressure_alarm,
                      dm1_fresh || mdi_low_oil_fresh, verified_low_oil,
                      reported[1], last_state[1]);
  publish_if_relevant(sk_low_voltage_alarm,
                      dm1_fresh || mdi_low_voltage_fresh, verified_low_voltage,
                      reported[2], last_state[2]);
  publish_if_relevant(sk_glow_plug_fault_alarm, dm1_fresh,
                      verified_glow_plug, reported[3], last_state[3]);
  publish_if_relevant(sk_engine_check_alarm,
                      dm1_fresh || mdi_engine_check_fresh,
                      verified_engine_check, reported[4], last_state[4]);
  if (heartbeat) last_publish_ms = now;
}

void set_alarm_states(bool over_temp, bool low_oil, bool low_voltage, bool glow_plug) {
  bool changed = over_temperature_alarm != over_temp ||
                 low_oil_pressure_alarm != low_oil ||
                 low_voltage_alarm != low_voltage ||
                 glow_plug_fault_alarm != glow_plug;

  over_temperature_alarm = over_temp;
  low_oil_pressure_alarm = low_oil;
  low_voltage_alarm = low_voltage;
  glow_plug_fault_alarm = glow_plug;

  if (changed) {
    publish_alarm_states();
  }
}

void publish_combined_alarm_states() {
  set_alarm_states(dm1_over_temperature || mdi_over_temperature,
                   dm1_low_oil || mdi_low_oil,
                   dm1_low_voltage || mdi_low_voltage,
                   dm1_glow_plug);
}

void check_dm1_timeout() {
  const uint32_t now = millis();
  const bool dm1_expired =
      last_dm1_ms != 0 && now - last_dm1_ms > kDm1TimeoutMs;
  const bool mdi_expired = last_mdi_warning_ms != 0 &&
                           now - last_mdi_warning_ms > kDm1TimeoutMs;

  if (dm1_expired) {
    dm1_available = false;
    last_dm1_ms = 0;
    dm1_over_temperature = false;
    dm1_low_oil = false;
    dm1_low_voltage = false;
    dm1_glow_plug = false;
    dm1_check_engine = false;
  }
  if (mdi_expired) {
    last_mdi_warning_ms = 0;
    mdi_over_temperature = false;
    mdi_low_oil = false;
    mdi_low_voltage = false;
    preheat_active = false;
    engine_starting = false;
    engine_stopping = false;
    system_fault = false;
    auxiliary_fault = false;
  }

  if (dm1_expired || mdi_expired) {
    const bool another_source_is_fresh =
        last_dm1_ms != 0 || (mdi_mapping_verified && last_mdi_warning_ms != 0);
    if (!another_source_is_fresh) {
      // Keep the last public alarm state latched but do not publish it again.
      // Signal K/XCover will see it become stale instead of receiving a false
      // "all clear". DTC values are likewise retained: zero means no DTC,
      // whereas a timeout means their state is unknown.
      return;
    }
    engine_check_alarm = dm1_check_engine || system_fault || auxiliary_fault;
    publish_combined_alarm_states();
    publish_alarm_states();
  }
}

struct DtcCode {
  uint32_t spn = 0;
  uint8_t fmi = 0;
  uint8_t occurrence_count = 0;
  uint8_t conversion_method = 0;
};

DtcCode decode_dm1_dtc(const uint8_t* data) {
  DtcCode dtc;
  dtc.spn = uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
            ((uint32_t(data[2]) & 0xe0) << 11);
  dtc.fmi = data[2] & 0x1f;
  dtc.occurrence_count = data[3] & 0x7f;
  dtc.conversion_method = (data[3] >> 7) & 0x01;
  return dtc;
}

void parse_dm1(const uint8_t* data, uint16_t len) {
  if (len < 2) {
    return;
  }
  last_dm1_ms = millis();
  dm1_available = true;

  bool over_temp = false;
  bool low_oil = false;
  bool low_voltage = false;
  bool glow_plug = false;

  uint16_t dtc_count = 0;
  uint32_t first_spn = 0;
  uint8_t first_fmi = 0;
  for (uint16_t offset = 2; offset + 3 < len; offset += 4) {
    bool empty_dtc = data[offset] == 0xff && data[offset + 1] == 0xff &&
                     data[offset + 2] == 0xff && data[offset + 3] == 0xff;
    if (empty_dtc) {
      continue;
    }

    DtcCode dtc = decode_dm1_dtc(&data[offset]);
    // decode_dm1_dtc() implements the version 4 SPN layout. A DTC that
    // announces a different conversion method packs its SPN differently, so
    // the decoded number would be wrong and would silently fail to match the
    // SPN 110/100/158/167/677/724 alarm rules below. Report it and skip it
    // rather than acting on a misdecoded code.
    if (dtc.conversion_method != 0) {
      Serial.printf(
          "DM1 DTC ignored: unsupported conversion method %u (raw %02x %02x "
          "%02x %02x)\n",
          dtc.conversion_method, data[offset], data[offset + 1],
          data[offset + 2], data[offset + 3]);
      continue;
    }
    if (dtc_count == 0) {
      first_spn = dtc.spn;
      first_fmi = dtc.fmi;
    }
    dtc_count++;
    Serial.printf("DM1 DTC spn=%lu fmi=%u oc=%u cm=%u\n", dtc.spn, dtc.fmi,
                  dtc.occurrence_count, dtc.conversion_method);

    if (dtc.spn == 110 && (dtc.fmi == 0 || dtc.fmi == 16)) {
      over_temp = true;
    } else if (dtc.spn == 100 && (dtc.fmi == 1 || dtc.fmi == 18)) {
      low_oil = true;
    } else if ((dtc.spn == 158 || dtc.spn == 167) &&
               (dtc.fmi == 1 || dtc.fmi == 18)) {
      low_voltage = true;
    } else if ((dtc.spn == 677 || dtc.spn == 724) && dtc.fmi == 5) {
      glow_plug = true;
    }
  }

  // Each two-bit lamp field uses 01=on, 00=off, 10/11=unavailable/error.
  const bool lamp_alarm = (data[0] & 0x03) == 0x01 ||
                          ((data[0] >> 2) & 0x03) == 0x01 ||
                          ((data[0] >> 4) & 0x03) == 0x01 ||
                          ((data[0] >> 6) & 0x03) == 0x01;
  dm1_over_temperature = over_temp;
  dm1_low_oil = low_oil;
  dm1_low_voltage = low_voltage;
  dm1_glow_plug = glow_plug;
  dm1_check_engine = lamp_alarm || dtc_count > 0;
  engine_check_alarm = dm1_check_engine || system_fault || auxiliary_fault;
  if (sk_active_dtc_count != nullptr) sk_active_dtc_count->set(float(dtc_count));
  if (sk_first_dtc_spn != nullptr) sk_first_dtc_spn->set(float(first_spn));
  if (sk_first_dtc_fmi != nullptr) sk_first_dtc_fmi->set(float(first_fmi));
  publish_combined_alarm_states();
  publish_alarm_states();
}

bool mask_active(uint64_t raw, uint64_t mask) {
  return mask != 0 && (raw & mask) == mask;
}

void parse_volvo_warnings(const uint8_t* data, uint8_t len) {
  if (len < 8) return;
  const uint32_t now = millis();
  last_mdi_warning_ms = now;
  mdi_detected = true;

#if !VOLVO_MDI_TEST_MODE
  // PGN 65417 is multiplexed: byte 0 selects the record and byte 1 is a
  // rolling counter. Do not flatten all records into a single 64-bit mask.
  // C1 is the first individual field verified against both the Volvo timing
  // rule and repeated voltage traces.
  if (data[0] == 0xC1) {
    mdi_low_voltage = (data[3] & 0x20U) != 0;
    publish_combined_alarm_states();
    publish_alarm_states();
    return;
  }
  note_alarm_frame(kPgnVolvoWarnings, data, len);
  return;
#else
  if (!mdi_mapping_verified) {
    note_alarm_frame(kPgnVolvoWarnings, data, len);
    return;
  }

  const uint64_t raw = le_u64(data);
  mdi_over_temperature = mask_active(raw, MDI_MASK_OVER_TEMP);
  mdi_low_oil = mask_active(raw, MDI_MASK_LOW_OIL);
  mdi_low_voltage = mask_active(raw, MDI_MASK_LOW_VOLTAGE);
  preheat_active = mask_active(raw, MDI_MASK_PREHEAT);
  engine_starting = mask_active(raw, MDI_MASK_STARTING);
  engine_stopping = mask_active(raw, MDI_MASK_STOPPING);
  system_fault = mask_active(raw, MDI_MASK_SYSTEM_FAULT);
  auxiliary_fault = mask_active(raw, MDI_MASK_AUXILIARY_FAULT);
  engine_check_alarm = dm1_check_engine || system_fault || auxiliary_fault;
  publish_combined_alarm_states();
  publish_alarm_states();
#endif
}

void parse_dash_display(const uint8_t* data, uint8_t len) {
  if (len < 2 || !valid_u8(data[1]) || sk_fuel_level == nullptr) return;
  sk_fuel_level->set((float(data[1]) * 0.4f) / 100.0f);
}

uint32_t pgn_from_tp_cm(const uint8_t* data) {
  return uint32_t(data[5]) | (uint32_t(data[6]) << 8) |
         (uint32_t(data[7]) << 16);
}

void reset_transport_session() {
  tp_session.active = false;
  tp_session.received = 0;
}

void parse_tp_cm(uint8_t source, const uint8_t* data, uint8_t len) {
  if (len < 8) return;
  const uint8_t control = data[0];
  const uint32_t target_pgn = pgn_from_tp_cm(data);
  if ((control != 0x20 && control != 0x10) || target_pgn != kPgnDm1) return;
  const uint16_t total_size = le_u16(data, 1);
  if (total_size < 2 || total_size > kTpBufferSize || data[3] == 0) {
    reset_transport_session();
    return;
  }
  tp_session.active = true;
  tp_session.source = source;
  tp_session.next_sequence = 1;
  tp_session.total_packets = data[3];
  tp_session.total_size = total_size;
  tp_session.received = 0;
  tp_session.target_pgn = target_pgn;
  tp_session.started_ms = millis();
}

void parse_tp_dt(uint8_t source, const uint8_t* data, uint8_t len) {
  if (!tp_session.active || source != tp_session.source || len < 2 ||
      millis() - tp_session.started_ms > kTpTimeoutMs ||
      data[0] != tp_session.next_sequence) {
    reset_transport_session();
    return;
  }
  for (uint8_t i = 1; i < len && tp_session.received < tp_session.total_size; i++) {
    tp_session.payload[tp_session.received++] = data[i];
  }
  tp_session.next_sequence++;
  if (tp_session.received >= tp_session.total_size) {
    parse_dm1(tp_session.payload, tp_session.total_size);
    reset_transport_session();
  }
}

void note_unknown_frame(uint32_t id, uint32_t pgn, const uint8_t* data, uint8_t len) {
  latest_unknown_pgn = pgn;
  unknown_frame_count++;

  UnknownPgnCounter* slot = nullptr;
  for (uint8_t i = 0; i < kMaxUnknownPgns; i++) {
    if (unknown_pgns[i].pgn == pgn) {
      slot = &unknown_pgns[i];
      break;
    }
    if (unknown_pgns[i].pgn == 0 && slot == nullptr) {
      slot = &unknown_pgns[i];
    }
  }

  if (slot != nullptr && slot->pgn == 0) {
    slot->pgn = pgn;
  }
  if (slot != nullptr) {
    slot->count++;
  }

  uint32_t seen_count = slot != nullptr ? slot->count : unknown_frame_count;
  if (seen_count != 1 && (seen_count % 50) != 0) {
    return;
  }

  Serial.printf("UNKNOWN PGN pgn=%lu count=%lu id=0x%08lx sa=0x%02x data=",
                pgn, seen_count, id, extract_source(id));
  for (uint8_t i = 0; i < len; i++) {
    Serial.printf("%02x", data[i]);
    if (i + 1 < len) {
      Serial.print(' ');
    }
  }
  Serial.println();
}

void remember_can_bitrate(uint16_t bitrate_kbps) {
#if !VOLVO_MDI_TEST_MODE
  if (saved_can_bitrate_kbps != 0 ||
      (bitrate_kbps != 250 && bitrate_kbps != 500)) {
    return;
  }
  Preferences preferences;
  if (!preferences.begin(kCanPreferencesNamespace, false)) {
    Serial.println("CAN bitrate confirmed but NVS could not be opened");
    return;
  }
  const size_t written =
      preferences.putUShort(kCanBitratePreferenceKey, bitrate_kbps);
  preferences.end();
  if (written == sizeof(uint16_t)) {
    saved_can_bitrate_kbps = bitrate_kbps;
    Serial.printf("CAN bitrate learned and saved permanently: %u kbps\n",
                  bitrate_kbps);
  } else {
    Serial.println("CAN bitrate confirmed but could not be saved to NVS");
  }
#endif
}

void handle_j1939_frame(const twai_message_t& msg) {
  if ((msg.flags & TWAI_MSG_FLAG_EXTD) == 0) {
    return;
  }

  uint32_t id = msg.identifier;
  uint32_t pgn = extract_pgn(id);
  uint8_t source = extract_source(id);
  diagnostic_web.note_j1939_frame(id, pgn, source, msg.data,
                                  msg.data_length_code);
  latest_pgn = pgn;
  frame_count++;
#if !VOLVO_MDI_TEST_MODE
  if (!can_bitrate_locked) {
    can_bitrate_locked = true;
    remember_can_bitrate(current_can_bitrate_kbps);
  }
#endif

  if (VOLVO_MDI_TRACE_ALL) {
    print_frame(id, pgn, msg.data, msg.data_length_code);
  }

  if (pgn == kPgnVolvoWarnings) {
    // A proprietary Volvo frame alone is not proof that its sender is the
    // engine ECU: the display at 0xF2 sends this PGN too.  Wait until EEC1 has
    // selected the engine source, then accept warnings only from that source.
    if (selected_engine_source == 0xff || source != selected_engine_source) {
      return;
    }
    parse_volvo_warnings(msg.data, msg.data_length_code);
    return;
  }
  if (pgn == kPgnTpCm) {
    if (source == selected_engine_source) {
      parse_tp_cm(source, msg.data, msg.data_length_code);
    }
    return;
  }
  if (pgn == kPgnTpDt) {
    parse_tp_dt(source, msg.data, msg.data_length_code);
    return;
  }

  const bool engine_pgn = pgn == kPgnEec1 || pgn == kPgnHours ||
                          pgn == kPgnTemp1 || pgn == kPgnFluidPressure ||
                          pgn == kPgnElectrical || pgn == kPgnDm1 ||
                          pgn == kPgnDashDisplay || pgn == kPgnShutdown;
  if (engine_pgn) {
    if (selected_engine_source == 0xff) {
      // Do not latch onto a malformed/unavailable EEC1 payload either.
      // A real SPN 190 is the evidence used for automatic source selection.
      if (pgn != kPgnEec1 || msg.data_length_code < 5 ||
          !valid_u16(le_u16(msg.data, 3)) ||
          !consider_engine_source(source)) {
        return;
      }
    } else if (source != selected_engine_source) {
      return;
    }
  }

  switch (pgn) {
    case kPgnEec1:
      parse_eec1(msg.data, msg.data_length_code);
      break;
    case kPgnHours:
      parse_hours(msg.data, msg.data_length_code);
      break;
    case kPgnTemp1:
      parse_temp1(msg.data, msg.data_length_code);
      break;
    case kPgnFluidPressure:
      parse_fluid_pressure(msg.data, msg.data_length_code);
      break;
    case kPgnElectrical:
      parse_electrical(msg.data, msg.data_length_code);
      break;
    case kPgnDashDisplay:
      parse_dash_display(msg.data, msg.data_length_code);
      break;
    case kPgnDm1:
      note_alarm_frame(pgn, msg.data, msg.data_length_code);
      parse_dm1(msg.data, msg.data_length_code);
      break;
    case kPgnShutdown:
      note_alarm_frame(pgn, msg.data, msg.data_length_code);
      note_unknown_frame(id, pgn, msg.data, msg.data_length_code);
      break;
    default:
      note_unknown_frame(id, pgn, msg.data, msg.data_length_code);
      break;
  }
}

void handle_can_frame(const twai_message_t& msg) {
  raw_can_frame_count++;
  if ((msg.flags & TWAI_MSG_FLAG_EXTD) != 0) {
    extended_can_frame_count++;
  } else {
    standard_can_frame_count++;
  }
  handle_j1939_frame(msg);
}

void update_timed_manual_rule(TimedManualRule& rule, bool valid,
                              bool condition, uint32_t threshold_ms,
                              uint32_t now) {
  const bool old_valid = rule.valid;
  const bool old_condition = rule.condition;
  const bool old_on = rule.on;
  const uint32_t old_threshold = rule.threshold_ms;
  rule.valid = valid;
  rule.condition = valid && condition;
  rule.threshold_ms = threshold_ms;

  if (!rule.valid || !rule.condition) {
    rule.timing = false;
    rule.started_ms = 0;
    rule.elapsed_ms = 0;
    rule.on = false;
  } else {
    if (!rule.timing || old_threshold != threshold_ms) {
      rule.timing = true;
      rule.started_ms = now;
    }
    rule.elapsed_ms = now - rule.started_ms;
    rule.on = rule.elapsed_ms >= threshold_ms;
  }

  if (old_valid != rule.valid || old_condition != rule.condition ||
      old_on != rule.on) {
    rule.changed_ms = now;
  }
}

void update_manual_alarm_rules(uint32_t now) {
  const bool rpm_valid = last_rpm_rx_ms != 0 &&
                         now - last_rpm_rx_ms <= kRuleSignalTimeoutMs &&
                         !isnan(latest_rpm);
  const bool oil_valid = last_oil_rx_ms != 0 &&
                         now - last_oil_rx_ms <= kRuleSignalTimeoutMs &&
                         !isnan(latest_oil_kpa);
  const bool temp_valid = last_temp_rx_ms != 0 &&
                          now - last_temp_rx_ms <= kRuleSignalTimeoutMs &&
                          !isnan(latest_coolant_c);
  // The workshop rule explicitly uses the voltage input at the MDI (SPN 158).
  // Do not silently replace it with alternator or battery voltage.
  const bool mdi_voltage_valid = last_mdi_supply_rx_ms != 0 &&
                                 now - last_mdi_supply_rx_ms <=
                                     kRuleSignalTimeoutMs &&
                                 !isnan(latest_mdi_supply_v);
  const bool engine_running = rpm_valid && latest_rpm >= kRuleRunningRpm;
  const uint32_t oil_delay = rpm_valid && latest_rpm >= 1000.0f ? 500 : 30000;
  update_timed_manual_rule(manual_oil_rule, rpm_valid && oil_valid,
                           latest_oil_kpa < kRuleOilThresholdKpa, oil_delay,
                           now);
  update_timed_manual_rule(manual_temp_rule, temp_valid,
                           latest_coolant_c > kRuleHighTemperatureC, 15000,
                           now);
  update_timed_manual_rule(manual_low_voltage_rule,
                           mdi_voltage_valid && rpm_valid,
                           engine_running &&
                               latest_mdi_supply_v <= kRuleLowVoltage,
                           10000, now);
  update_timed_manual_rule(manual_high_voltage_rule, mdi_voltage_valid,
                           latest_mdi_supply_v >= kRuleHighVoltage, 30000,
                           now);
}

void update_diagnostic_runtime() {
  DiagnosticRuntimeState state;
  state.test_mode = VOLVO_MDI_TEST_MODE;
  state.can_started = can_started;
  state.can_locked = can_bitrate_locked;
  state.can_bitrate_saved = saved_can_bitrate_kbps != 0;
  state.mdi_detected = mdi_detected;
  state.mapping_verified = mdi_mapping_verified;
  state.dm1_available = dm1_available;
  const uint32_t now = millis();
  update_manual_alarm_rules(now);
  state.alarm_data_fresh =
      (last_dm1_ms != 0 && now - last_dm1_ms <= kDm1TimeoutMs) ||
      (mdi_mapping_verified && last_mdi_warning_ms != 0 &&
       now - last_mdi_warning_ms <= kDm1TimeoutMs);
  state.over_temperature = over_temperature_alarm;
  state.low_oil = low_oil_pressure_alarm;
  state.low_voltage = low_voltage_alarm;
  state.engine_check = engine_check_alarm;
  state.rule_oil_valid = manual_oil_rule.valid;
  state.rule_oil_condition = manual_oil_rule.condition;
  state.rule_oil_on = manual_oil_rule.on;
  state.rule_oil_elapsed_ms = manual_oil_rule.elapsed_ms;
  state.rule_oil_threshold_ms = manual_oil_rule.threshold_ms;
  state.rule_oil_changed_ms = manual_oil_rule.changed_ms;
  state.rule_temp_valid = manual_temp_rule.valid;
  state.rule_temp_condition = manual_temp_rule.condition;
  state.rule_temp_on = manual_temp_rule.on;
  state.rule_temp_elapsed_ms = manual_temp_rule.elapsed_ms;
  state.rule_temp_changed_ms = manual_temp_rule.changed_ms;
  state.rule_low_voltage_valid = manual_low_voltage_rule.valid;
  state.rule_low_voltage_condition = manual_low_voltage_rule.condition;
  state.rule_low_voltage_on = manual_low_voltage_rule.on;
  state.rule_low_voltage_elapsed_ms = manual_low_voltage_rule.elapsed_ms;
  state.rule_low_voltage_changed_ms = manual_low_voltage_rule.changed_ms;
  state.rule_high_voltage_valid = manual_high_voltage_rule.valid;
  state.rule_high_voltage_condition = manual_high_voltage_rule.condition;
  state.rule_high_voltage_on = manual_high_voltage_rule.on;
  state.rule_high_voltage_elapsed_ms = manual_high_voltage_rule.elapsed_ms;
  state.rule_high_voltage_changed_ms = manual_high_voltage_rule.changed_ms;
  state.can_bitrate_kbps = current_can_bitrate_kbps;
  state.engine_source = selected_engine_source;
  state.can_raw_frame_count = raw_can_frame_count;
  state.can_extended_frame_count = extended_can_frame_count;
  state.can_standard_frame_count = standard_can_frame_count;
  state.frame_count = frame_count;
  state.unknown_frame_count = unknown_frame_count;
  state.latest_pgn = latest_pgn;
  state.rpm = latest_rpm;
  state.coolant_c = latest_coolant_c;
  state.alternator_v = latest_alternator_v;
  state.battery_v = latest_battery_v;
  state.mdi_supply_v = latest_mdi_supply_v;
  state.oil_kpa = latest_oil_kpa;

  auto ws_client = sensesp_app != nullptr ? sensesp_app->get_ws_client() : nullptr;
  if (ws_client != nullptr) {
    const String status = ws_client->is_connected()
                              ? "Conectado"
                              : ws_client->get_connection_status();
    strlcpy(state.signalk_status, status.c_str(), sizeof(state.signalk_status));
    strlcpy(state.signalk_server, ws_client->get_server_address().c_str(),
            sizeof(state.signalk_server));
    state.signalk_port = ws_client->get_server_port();
  }
  diagnostic_web.set_runtime_state(state);
}

void poll_can() {
  twai_message_t msg;
  while (twai_receive(&msg, 0) == ESP_OK) {
    handle_can_frame(msg);
  }
}

void inject_test_frame(uint32_t pgn, const uint8_t* data, uint8_t len) {
  twai_message_t msg = make_test_frame(pgn, data, len);
  handle_can_frame(msg);
}

void encode_dm1_dtc(uint8_t* out, uint32_t spn, uint8_t fmi, uint8_t occurrence_count = 1,
                    uint8_t conversion_method = 0) {
  out[0] = spn & 0xff;
  out[1] = (spn >> 8) & 0xff;
  out[2] = ((spn >> 11) & 0xe0) | (fmi & 0x1f);
  out[3] = ((conversion_method & 0x01) << 7) | (occurrence_count & 0x7f);
}

enum class TestScenario : uint8_t {
  kNormal = 0,
  kOverTemperatureDm1AndValue,
  kOverTemperatureValueOnly,
  kLowOilDm1AndValue,
  kLowOilValueOnly,
  kLowVoltageDm1AndValue,
  kLowVoltageValueOnly,
  kGlowPlugDm1,
  kPreheatMdi,
  kMdiSystemFault,
  kDoneNormal,
};

const char* test_scenario_name(TestScenario scenario) {
  switch (scenario) {
    case TestScenario::kNormal:
      return "normal";
    case TestScenario::kOverTemperatureDm1AndValue:
      return "overtemperature: DM1 + coolant high";
    case TestScenario::kOverTemperatureValueOnly:
      return "overtemperature: coolant high only";
    case TestScenario::kLowOilDm1AndValue:
      return "low oil pressure: DM1 + pressure low";
    case TestScenario::kLowOilValueOnly:
      return "low oil pressure: pressure low only";
    case TestScenario::kLowVoltageDm1AndValue:
      return "low voltage: DM1 + alternator low";
    case TestScenario::kLowVoltageValueOnly:
      return "low voltage: alternator low only";
    case TestScenario::kGlowPlugDm1:
      return "glow plug fault: DM1";
    case TestScenario::kPreheatMdi:
      return "preheat active: Volvo MDI warning";
    case TestScenario::kMdiSystemFault:
      return "Volvo MDI system fault";
    case TestScenario::kDoneNormal:
      return "done: normal";
  }
  return "unknown";
}

TestScenario current_test_scenario() {
  uint32_t step = millis() / kTestScenarioMs;
  if (step >= uint32_t(TestScenario::kDoneNormal)) {
    return TestScenario::kDoneNormal;
  }
  return TestScenario(step);
}

void inject_dm1_code(uint32_t spn = 0, uint8_t fmi = 0, bool active = false) {
  uint8_t dm1[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  if (active) {
    encode_dm1_dtc(&dm1[2], spn, fmi);
  }
  inject_test_frame(kPgnDm1, dm1, sizeof(dm1));
}

void inject_multipacket_dm1() {
  uint8_t payload[10] = {0x04, 0xff};  // amber warning lamp + two DTCs
  encode_dm1_dtc(&payload[2], 724, 5);
  encode_dm1_dtc(&payload[6], 100, 18);

  uint8_t cm[8] = {0x20, sizeof(payload), 0x00, 0x02, 0xff,
                   uint8_t(kPgnDm1 & 0xff), uint8_t((kPgnDm1 >> 8) & 0xff),
                   uint8_t((kPgnDm1 >> 16) & 0xff)};
  inject_test_frame(kPgnTpCm, cm, sizeof(cm));
  uint8_t dt1[8] = {0x01, payload[0], payload[1], payload[2], payload[3],
                    payload[4], payload[5], payload[6]};
  uint8_t dt2[8] = {0x02, payload[7], payload[8], payload[9], 0xff, 0xff, 0xff, 0xff};
  inject_test_frame(kPgnTpDt, dt1, sizeof(dt1));
  inject_test_frame(kPgnTpDt, dt2, sizeof(dt2));
}

void inject_test_j1939() {
  static TestScenario last_scenario = TestScenario::kDoneNormal;
  static bool first_run = true;

  TestScenario scenario = current_test_scenario();
  if (first_run || scenario != last_scenario) {
    first_run = false;
    last_scenario = scenario;
    Serial.printf("TEST SCENARIO: %s\n", test_scenario_name(scenario));
  }

  float rpm = 900.0f;
  float coolant_c = 78.0f;
  float oil_kpa = 260.0f;
  float alternator_v = 14.2f;
  bool send_dm1 = false;
  uint32_t dm1_spn = 0;
  uint8_t dm1_fmi = 0;
  uint8_t mdi_warning = 0;

  switch (scenario) {
    case TestScenario::kOverTemperatureDm1AndValue:
      coolant_c = 106.0f;
      send_dm1 = true;
      dm1_spn = 110;
      dm1_fmi = 0;
      break;
    case TestScenario::kOverTemperatureValueOnly:
      coolant_c = 106.0f;
      break;
    case TestScenario::kLowOilDm1AndValue:
      oil_kpa = 20.0f;
      send_dm1 = true;
      dm1_spn = 100;
      dm1_fmi = 1;
      break;
    case TestScenario::kLowOilValueOnly:
      oil_kpa = 20.0f;
      break;
    case TestScenario::kLowVoltageDm1AndValue:
      alternator_v = 11.2f;
      send_dm1 = true;
      dm1_spn = 167;
      dm1_fmi = 1;
      break;
    case TestScenario::kLowVoltageValueOnly:
      alternator_v = 11.2f;
      break;
    case TestScenario::kGlowPlugDm1:
      break;
    case TestScenario::kPreheatMdi:
      rpm = 0.0f;
      coolant_c = 5.0f;
      alternator_v = 12.2f;
      mdi_warning = uint8_t(MDI_MASK_PREHEAT);
      break;
    case TestScenario::kMdiSystemFault:
      mdi_warning = uint8_t(MDI_MASK_SYSTEM_FAULT);
      break;
    case TestScenario::kNormal:
    case TestScenario::kDoneNormal:
      break;
  }

  uint16_t raw_rpm = uint16_t(rpm / 0.125f);
  uint8_t eec1[8] = {0xff, 0xff, 165, uint8_t(raw_rpm & 0xff),
                     uint8_t(raw_rpm >> 8), 0xff, 0xff, 0xff};
  inject_test_frame(kPgnEec1, eec1, sizeof(eec1));

  uint32_t raw_hours = uint32_t((1234.5f + millis() / 3600000.0f) / 0.05f);
  uint8_t hours[8] = {uint8_t(raw_hours & 0xff), uint8_t((raw_hours >> 8) & 0xff),
                      uint8_t((raw_hours >> 16) & 0xff), uint8_t(raw_hours >> 24),
                      0xff, 0xff, 0xff, 0xff};
  inject_test_frame(kPgnHours, hours, sizeof(hours));

  uint8_t temp1[8] = {uint8_t(coolant_c + 40.0f), 0xff, 0xff, 0xff,
                      0xff, 0xff, 0xff, 0xff};
  inject_test_frame(kPgnTemp1, temp1, sizeof(temp1));

  uint8_t pressure[8] = {0xff, 0xff, 0xff, uint8_t(oil_kpa / 4.0f),
                         0xff, 0xff, 0xff, 0xff};
  inject_test_frame(kPgnFluidPressure, pressure, sizeof(pressure));

  uint16_t raw_voltage = uint16_t(alternator_v / 0.05f);
  uint8_t electrical[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                           uint8_t(raw_voltage & 0xff),
                           uint8_t(raw_voltage >> 8)};
  inject_test_frame(kPgnElectrical, electrical, sizeof(electrical));

  if (scenario == TestScenario::kGlowPlugDm1) {
    inject_multipacket_dm1();
  } else {
    inject_dm1_code(dm1_spn, dm1_fmi, send_dm1);
  }

  uint8_t warning[8] = {mdi_warning, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  inject_test_frame(kPgnVolvoWarnings, warning, sizeof(warning));

  if (scenario == TestScenario::kDoneNormal) {
    uint8_t unknown[8] = {0x34, 0x12, 0x78, 0x56, 0xaa, 0x55, 0x00, 0xff};
    inject_test_frame(kPgnTestUnknown, unknown, sizeof(unknown));
  }
}

void poll_temperatures() {
  dallas.requestTemperatures();

  float temp0 = dallas.getTempCByIndex(0);
  float temp1 = dallas.getTempCByIndex(1);
  float temp2 = dallas.getTempCByIndex(2);

  if (temp0 != DEVICE_DISCONNECTED_C && sk_engine_room_temp != nullptr) {
    sk_engine_room_temp->set(temp0 + 273.15f);
  }
  if (temp1 != DEVICE_DISCONNECTED_C && sk_exhaust_temp != nullptr) {
    sk_exhaust_temp->set(temp1 + 273.15f);
  }
  if (temp2 != DEVICE_DISCONNECTED_C && sk_aux_temp != nullptr) {
    sk_aux_temp->set(temp2 + 273.15f);
  }
}

void draw_display_value(uint8_t row, const char* label, float value, const char* unit) {
  display.setCursor(0, row * 8);
  if (isnan(value)) {
    display.printf("%s: --", label);
  } else {
    display.printf("%s: %.1f %s", label, value, unit);
  }
}

void update_display() {
  if (!display_ok) {
    return;
  }

  display.clearDisplay();
  display.setCursor(0, 0);
#if VOLVO_MDI_TEST_MODE
  display.print("Volvo MDI TEST");
#else
  display.print("Volvo MDI LIVE");
#endif
  display.setCursor(0, 8);
  display.printf("IP: %s", WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "...");
  draw_display_value(2, "RPM", latest_rpm, "");
  draw_display_value(3, "Cool", latest_coolant_c, "C");
  draw_display_value(4, "Volt",
                     !isnan(latest_alternator_v)
                         ? latest_alternator_v
                         : (!isnan(latest_mdi_supply_v) ? latest_mdi_supply_v
                                                        : latest_battery_v),
                     "V");
  draw_display_value(5, "Oil", latest_oil_kpa, "kPa");
  display.setCursor(0, 48);
  display.printf("PGN:%lu U:%lu", latest_pgn, latest_unknown_pgn);
  display.setCursor(0, 56);
  display.printf("F:%lu Unk:%lu", frame_count, unknown_frame_count);
  display.display();
}

void setup_display() {
  display_i2c.begin(kI2cSdaPin, kI2cSclPin);
  display_ok = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  if (!display_ok) {
    Serial.println("SSD1306 not found at 0x3C; continuing without display");
    return;
  }

  display.setRotation(2);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Volvo MDI boot");
  display.display();
}

bool start_can_at(uint16_t bitrate_kbps) {
  if (can_started) {
    twai_stop();
    twai_driver_uninstall();
    can_started = false;
  }
  twai_general_config_t general_config =
      TWAI_GENERAL_CONFIG_DEFAULT(kCanTxPin, kCanRxPin, TWAI_MODE_LISTEN_ONLY);
  // The ESP-IDF TWAI timing macros expand to braced initializers. They can
  // initialize a typed object, but cannot be used directly as operands of a
  // C++ conditional expression.
  const twai_timing_config_t timing_250 = TWAI_TIMING_CONFIG_250KBITS();
  const twai_timing_config_t timing_500 = TWAI_TIMING_CONFIG_500KBITS();
  const twai_timing_config_t timing_config =
      bitrate_kbps == 500 ? timing_500 : timing_250;
  twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&general_config, &timing_config, &filter_config);
  if (err != ESP_OK) {
    Serial.printf("TWAI install failed: %d\n", err);
    return false;
  }

  err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("TWAI start failed: %d\n", err);
    twai_driver_uninstall();
    return false;
  }

  can_started = true;
  current_can_bitrate_kbps = bitrate_kbps;
  can_probe_started_ms = millis();
  Serial.printf("TWAI/CAN listen-only probing at %u kbps\n", bitrate_kbps);
  return true;
}

void auto_detect_can_bitrate() {
#if !VOLVO_MDI_TEST_MODE
  // Once a bitrate has been confirmed and stored, absence of traffic means a
  // contact/wiring/power issue, not permission to silently change bitrate.
  if (saved_can_bitrate_kbps != 0 || can_bitrate_locked ||
      millis() - can_probe_started_ms < kCanProbeWindowMs) {
    return;
  }
  start_can_at(current_can_bitrate_kbps == 250 ? 500 : 250);
#endif
}

uint16_t load_saved_can_bitrate() {
  Preferences preferences;
  if (!preferences.begin(kCanPreferencesNamespace, true)) return 0;
  const uint16_t bitrate =
      preferences.getUShort(kCanBitratePreferenceKey, 0);
  preferences.end();
  return bitrate == 250 || bitrate == 500 ? bitrate : 0;
}

// The provisioning AP stays up permanently, so its password must not be
// derivable from anything the device broadcasts. Deriving it from the eFuse MAC
// made it effectively public: the SoftAP BSSID in every beacon frame is that
// same MAC plus a fixed, documented offset, so anyone scanning for WiFi could
// reconstruct the password without guessing. Hashing would not help either,
// since both the algorithm and its input would still be public. Generate a
// random secret once instead and keep it in NVS.
void generate_ap_password(char* out, uint8_t length) {
  static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
  const uint8_t alphabet_size = sizeof(kAlphabet) - 1;
  // Reject the tail of the byte range that would bias the modulo mapping.
  const uint8_t limit = uint8_t(256 / alphabet_size) * alphabet_size;
  uint8_t written = 0;
  while (written < length) {
    const uint8_t candidate = uint8_t(esp_random() & 0xff);
    if (candidate >= limit) continue;
    out[written++] = kAlphabet[candidate % alphabet_size];
  }
  out[written] = '\0';
}

// SensESP treats the builder's access point password as a default that only
// applies when no configuration has been saved yet: once the web UI writes
// /System/WiFi Settings, the saved password is what the soft AP actually uses.
// Reading it back is the only way to keep the serial banner, the OTA password
// and the diagnostic firmware-upload password from drifting apart from the
// password the device is really broadcasting.
bool load_saved_ap_password(char* out, size_t out_size) {
  const String hash_path = String("/") + Base64Sha1("/System/WiFi Settings");
  if (!SPIFFS.exists(hash_path)) return false;
  File file = SPIFFS.open(hash_path, FILE_READ);
  if (!file) return false;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) return false;
  const char* saved = doc["apSettings"]["password"];
  if (saved == nullptr || strlen(saved) < kMinApPasswordLength) return false;
  strlcpy(out, saved, out_size);
  return true;
}

bool load_or_create_ap_password(char* out, size_t out_size) {
  if (out_size < size_t(kGeneratedApPasswordLength) + 1) return false;
  Preferences preferences;
  if (!preferences.begin(kCanPreferencesNamespace, false)) return false;
  const size_t stored =
      preferences.getString(kApPasswordPreferenceKey, out, out_size);
  if (stored >= kMinApPasswordLength) {
    preferences.end();
    return true;
  }
  generate_ap_password(out, kGeneratedApPasswordLength);
  const bool saved = preferences.putString(kApPasswordPreferenceKey, out) > 0;
  preferences.end();
  return saved;
}

// Called from the diagnostic web task. Preferences/NVS is thread safe, and the
// device restarts right after, so no CAN state is touched here.
bool forget_saved_can_bitrate() {
  Preferences preferences;
  if (!preferences.begin(kCanPreferencesNamespace, false)) return false;
  const bool removed = preferences.remove(kCanBitratePreferenceKey);
  preferences.end();
  if (removed) {
    Serial.println("Saved CAN bitrate cleared; the 250/500 probe runs again on the next boot");
  }
  return removed;
}

void setup_can() {
#if VOLVO_MDI_TEST_MODE
  Serial.println("VOLVO_MDI_TEST_MODE enabled: CAN hardware is not started; synthetic J1939 frames are injected internally");
  return;
#else
  saved_can_bitrate_kbps = load_saved_can_bitrate();
  if (saved_can_bitrate_kbps != 0) {
    Serial.printf("Using permanently saved CAN bitrate: %u kbps\n",
                  saved_can_bitrate_kbps);
    start_can_at(saved_can_bitrate_kbps);
  } else {
    // First commissioning only: probe both supported rates until a valid
    // extended J1939 frame confirms the bus timing.
    Serial.println("No saved CAN bitrate; starting one-time 250/500 probe");
    start_can_at(250);
  }
#endif
}

void setup() {
  Serial.begin(115200);
  delay(100);
  SetupLogging();

#if VOLVO_MDI_TEST_MODE
  Serial.println("BOOT MODE: TEST - synthetic J1939 demo frames will be generated");
#else
  Serial.println("BOOT MODE: LIVE - listening to real Volvo MDI/J1939 CAN frames");
#endif

  setup_display();

  SensESPAppBuilder builder;
  char generated_ap_password[24] = {};
  const char* ap_password = WIFI_AP_PASSWORD;
  if (strlen(ap_password) < kMinApPasswordLength) {
    if (!load_or_create_ap_password(generated_ap_password,
                                    sizeof(generated_ap_password))) {
      generate_ap_password(generated_ap_password, kGeneratedApPasswordLength);
      Serial.println(
          "AP password could not be stored in NVS; using a temporary one valid "
          "for this boot only");
    }
    ap_password = generated_ap_password;
  }
  // A password saved through the web UI overrides the builder default inside
  // SensESP. Adopt it here too, so the banner below, the OTA password and the
  // diagnostic upload password all name the password the AP really uses.
  char effective_ap_password[24] = {};
  if (load_saved_ap_password(effective_ap_password,
                             sizeof(effective_ap_password))) {
    Serial.println(
        "Access point password: taken from the saved web configuration");
  } else {
    strlcpy(effective_ap_password, ap_password, sizeof(effective_ap_password));
    Serial.printf("Provisioning AP password: %s\n", effective_ap_password);
  }
  strlcpy(ota_password, effective_ap_password, sizeof(ota_password));
  builder.set_hostname("sh-esp32-volvo-mdi")
      ->set_wifi_access_point("sh-esp32-volvo-mdi", ap_password)
      // GPIO0 is the SH-ESP32 boot button. SensESP wires it to restart on a
      // short press, wipe the WiFi configuration after one second and format
      // the filesystem after five. On a unit installed in an engine room that
      // is an unattended way to lose the boat's network settings.
      ->set_button_pin(-1)
      ->enable_ota(ota_password);

  if (strlen(WIFI_SSID) > 0) {
    builder.set_wifi_client(WIFI_SSID, WIFI_PASSWORD);
  }

  if (strlen(SIGNALK_HOST) > 0) {
    builder.set_sk_server(SIGNALK_HOST, SIGNALK_PORT);
  }

  sensesp_app = builder.get_app();

  sk_rpm = new SKOutputFloat("propulsion.main.revolutions", "/volvo_mdi/rpm",
                             new SKMetadata("Hz", "Engine speed"));
  sk_hours = new SKOutputFloat("propulsion.main.runTime", "/volvo_mdi/run_time",
                               new SKMetadata("s", "Engine run time"));
  sk_coolant_temp = new SKOutputFloat("propulsion.main.coolantTemperature", "/volvo_mdi/coolant_temp",
                                      new SKMetadata("K", "Coolant temperature"));
  sk_alternator_voltage = new SKOutputFloat("propulsion.main.alternatorVoltage",
                                            "/volvo_mdi/alternator_voltage",
                                            new SKMetadata("V", "Alternator voltage"));
  sk_start_battery_voltage = new SKOutputFloat("electrical.batteries.start.voltage",
                                               "/volvo_mdi/start_battery_voltage",
                                               new SKMetadata("V", "Start battery voltage"));
  sk_mdi_supply_voltage = new SKOutputFloat("propulsion.main.volvoMdi.supplyVoltage",
                                            "/volvo_mdi/supply_voltage",
                                            new SKMetadata("V", "MDI supply voltage"));
  sk_oil_pressure = new SKOutputFloat("propulsion.main.oilPressure", "/volvo_mdi/oil_pressure",
                                      new SKMetadata("Pa", "Oil pressure"));
  sk_fuel_level = new SKOutputFloat("tanks.fuel.main.currentLevel",
                                    "/volvo_mdi/fuel_level",
                                    new SKMetadata("ratio", "Fuel level from MDI"));
  sk_over_temperature_alarm = new SKOutputBool("propulsion.main.overTemperatureAlarm",
                                               "/volvo_mdi/over_temperature_alarm");
  sk_low_oil_pressure_alarm = new SKOutputBool("propulsion.main.lowOilPressureAlarm",
                                               "/volvo_mdi/low_oil_pressure_alarm");
  sk_low_voltage_alarm = new SKOutputBool("propulsion.main.lowVoltageAlarm",
                                          "/volvo_mdi/low_voltage_alarm");
  sk_glow_plug_fault_alarm = new SKOutputBool("propulsion.main.glowPlugFaultAlarm",
                                              "/volvo_mdi/glow_plug_fault_alarm");
  sk_engine_check_alarm = new SKOutputBool("propulsion.main.engineCheckAlarm",
                                           "/volvo_mdi/engine_check_alarm");
  // Proprietary Volvo status fields, source-selection internals and raw
  // diagnostics stay on the ESP32 web page/capture. They are deliberately
  // not exposed as Signal K paths until their meaning has been verified.
  sk_active_dtc_count = new SKOutputFloat("propulsion.main.volvoMdi.activeDtcCount",
                                          "/volvo_mdi/active_dtc_count");
  sk_first_dtc_spn = new SKOutputFloat("propulsion.main.volvoMdi.firstDtcSpn",
                                       "/volvo_mdi/first_dtc_spn");
  sk_first_dtc_fmi = new SKOutputFloat("propulsion.main.volvoMdi.firstDtcFmi",
                                       "/volvo_mdi/first_dtc_fmi");

  sk_engine_room_temp = new SKOutputFloat("environment.engineRoom.temperature",
                                          "/onewire/engine_room_temperature",
                                          new SKMetadata("K", "Engine room temperature"));
  sk_exhaust_temp = new SKOutputFloat("propulsion.main.exhaustTemperature",
                                      "/onewire/exhaust_temperature",
                                      new SKMetadata("K", "Exhaust temperature"));
  sk_aux_temp = new SKOutputFloat("environment.refrigerator.temperature",
                                  "/onewire/aux_temperature",
                                  new SKMetadata("K", "Auxiliary temperature"));

  dallas.begin();
  setup_can();
  diagnostic_web.set_can_bitrate_reset_handler(forget_saved_can_bitrate);
  diagnostic_web.begin(8080, ota_password);
  Serial.println("Diagnostic dashboard: http://<ESP32-IP>:8080/");

#if VOLVO_MDI_TEST_MODE
  event_loop()->onRepeat(1000, inject_test_j1939);
#else
  event_loop()->onRepeat(10, poll_can);
#endif
  event_loop()->onRepeat(5000, poll_temperatures);
  event_loop()->onRepeat(1000, update_display);
  event_loop()->onRepeat(1000, check_dm1_timeout);
  event_loop()->onRepeat(5000, check_engine_source_timeout);
  event_loop()->onRepeat(500, auto_detect_can_bitrate);
  event_loop()->onRepeat(1000, publish_alarm_states);
  event_loop()->onRepeat(500, update_diagnostic_runtime);
  event_loop()->onDelay(1500, publish_alarm_states);
  event_loop()->onDelay(250, update_diagnostic_runtime);
}

void loop() {
  event_loop()->tick();
}
