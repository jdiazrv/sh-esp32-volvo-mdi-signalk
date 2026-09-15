#pragma once

#include <stdint.h>

// Pure CAN capture filtering: no Arduino, clock, filesystem, or decoder state.
// Compare frames in separate (PGN, source, proprietary subtype) slots. Update
// the previous frame after every observation, even when this returns false.
namespace capture_policy {

struct Decision {
  uint8_t mask;
  bool record;
  bool telemetry_transition;
};

namespace detail {

// The exact 1000 RPM sample has its own band: the manual describes below and
// above 1000, so retain both arrival at and departure from this boundary.
inline uint8_t rpm_band(const uint8_t* data, uint8_t len) {
  if (len < 5) return 0;  // No complete SPN 190.
  const uint16_t raw = uint16_t(data[3]) | (uint16_t(data[4]) << 8);
  if (raw >= 0xFB00) return 0;  // Reserved / error / unavailable.
  if (raw == 0) return 1;
  if (raw < 8000) return 2;  // 0.125 RPM per bit.
  if (raw == 8000) return 3;
  return 4;
}

inline uint8_t coolant_band(const uint8_t* data, uint8_t len) {
  if (len == 0 || data[0] >= 0xFB) return 0;
  // SPN 110 is raw - 40 degrees C; 0x5A is exactly 50 degrees C.
  return data[0] < 0x5A ? 1 : 2;
}

}  // namespace detail

inline Decision decide(uint32_t pgn, uint8_t source, const uint8_t* data,
                       uint8_t len, const uint8_t* previous,
                       uint8_t previous_len, bool baseline,
                       uint8_t raw_changed_mask) {
  // A first observation is useful even with no payload or no significant bits.
  if (baseline) return {raw_changed_mask, true, false};

  uint8_t mask = raw_changed_mask;
  bool telemetry_transition = false;
  if (pgn == 61444) {
    constexpr uint8_t kRpmBytes = (1U << 3) | (1U << 4);
    telemetry_transition =
        detail::rpm_band(data, len) != detail::rpm_band(previous, previous_len);
    if (!telemetry_transition) mask &= uint8_t(~kRpmBytes);
  }
  if (pgn == 65262) {
    telemetry_transition = detail::coolant_band(data, len) !=
                           detail::coolant_band(previous, previous_len);
    // Ignore stable coolant variation, not other ET1 bytes whose information
    // may matter to an investigation. Always keep a complete recorded frame.
    if (!telemetry_transition) mask &= uint8_t(~1U);
  }
  if (pgn == 65417 || pgn == 65420) {
    mask &= uint8_t(~(1U << 1));  // Observed rolling counter, zero-based byte 1.
    // Only this known display heartbeat has the second observed counter.
    // Unknown emitters/subtypes must remain visible, including other F2 data.
    if (pgn == 65417 && source == 0xF2 && len > 0 && data[0] == 0xF3) {
      mask &= uint8_t(~(1U << 3));
    }
  }
  // Length transitions can be meaningful even when the current payload is
  // empty. There is intentionally no time or same-mask throttle: A->B->A must
  // yield two rows regardless of how quickly the transitions occur.
  return {mask, len != previous_len || mask != 0, telemetry_transition};
}

}  // namespace capture_policy
