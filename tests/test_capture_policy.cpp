#include "../src/capture_policy.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

using Frame = std::array<uint8_t, 8>;

static uint8_t changed(const Frame& before, const Frame& after) {
  uint8_t mask = 0;
  for (unsigned i = 0; i < after.size(); ++i) {
    if (before[i] != after[i]) mask |= uint8_t(1U << i);
  }
  return mask;
}

static capture_policy::Decision decide(uint32_t pgn, uint8_t source,
                                      const Frame& before, const Frame& after) {
  return capture_policy::decide(pgn, source, after.data(), after.size(),
                                before.data(), before.size(), false,
                                changed(before, after));
}

static Frame rpm(uint16_t raw) {
  Frame frame{};
  frame[3] = uint8_t(raw);
  frame[4] = uint8_t(raw >> 8);
  return frame;
}

static void test_baselines_and_lengths() {
  const Frame frame{};
  auto result = capture_policy::decide(61444, 0, frame.data(), 8, nullptr, 0,
                                       true, 0xFF);
  assert(result.record && result.mask == 0xFF);
  result = capture_policy::decide(65262, 0, nullptr, 0, nullptr, 0, true, 0);
  assert(result.record && result.mask == 0);
  result = capture_policy::decide(65262, 0, nullptr, 0, nullptr, 0, false, 0);
  assert(!result.record);
  result = capture_policy::decide(65417, 0, nullptr, 0, frame.data(), 8,
                                  false, 0);
  assert(result.record);  // No mask bit can encode a disappeared payload.
  result = capture_policy::decide(61444, 0, frame.data(), 5, frame.data(), 4,
                                  false, 0);
  assert(result.record && result.telemetry_transition);
}

static void test_rpm() {
  assert(!decide(61444, 0, rpm(6400), rpm(6410)).record);  // Stable 800 RPM.
  assert(!decide(61444, 0, rpm(9000), rpm(10000)).record);
  for (const auto pair : {std::array<uint16_t, 2>{0, 1}, {1, 0},
                         {7999, 8000}, {8000, 8001}, {8001, 8000},
                         {8000, 7999}, {7999, 8001}, {8001, 7999},
                         {0xFAFF, 0xFB00}, {0xFB00, 0xFAFF},
                         {0xFFFF, 0}, {0, 0xFFFF}}) {
    const auto result = decide(61444, 0, rpm(pair[0]), rpm(pair[1]));
    assert(result.record && result.telemetry_transition && result.mask != 0);
  }
  assert(!decide(61444, 0, rpm(0xFB00), rpm(0xFFFF)).record);
  Frame after = rpm(6410);
  after[2] = 0x20;
  const auto result = decide(61444, 0, rpm(6400), after);
  assert(result.record && result.mask == (1U << 2));
}

static void test_coolant() {
  Frame before{};
  Frame after{};
  before[0] = 89;
  after[0] = 90;
  auto result = decide(65262, 0, before, after);
  assert(result.record && result.telemetry_transition && result.mask == 1);
  assert(decide(65262, 0, after, before).record);
  before[0] = 88;
  assert(!decide(65262, 0, before, Frame{89}).record);
  before[0] = 90;
  after[0] = 91;
  assert(!decide(65262, 0, before, after).record);
  before[0] = 0xFA;
  after[0] = 0xFB;
  assert(decide(65262, 0, before, after).telemetry_transition);
  assert(decide(65262, 0, after, before).record);
  before[0] = 0xFB;
  after[0] = 0xFF;
  assert(!decide(65262, 0, before, after).record);
  after[2] = 0x20;
  result = decide(65262, 0, before, after);
  assert(result.record && result.mask == (1U << 2));
}

static void test_proprietary_and_rapid_reversal() {
  Frame before{0xC1, 1, 0xBC, 0};
  Frame after{0xC1, 2, 0xBC, 0};
  assert(!decide(65417, 0, before, after).record);
  assert(!decide(65420, 0, before, after).record);
  after[3] = 0x20;
  auto result = decide(65417, 0, before, after);
  assert(result.record && result.mask == (1U << 3));
  // The reversed transition changes exactly the same byte: retain it too.
  result = decide(65417, 0, after, before);
  assert(result.record && result.mask == (1U << 3));
  assert(decide(65417, 0xA3, before, after).record);  // Unknown emitter.
  assert(decide(65417, 0xF2, before, after).record);  // Not subtype F3.

  before = {0xF3, 1, 0x10, 2};
  after = {0xF3, 2, 0x10, 3};
  assert(!decide(65417, 0xF2, before, after).record);
  assert(decide(65417, 0, before, after).record);
  assert(decide(65420, 0xF2, before, after).record);
  after[2] = 0x20;
  result = decide(65417, 0xF2, before, after);
  assert(result.record && result.mask == (1U << 2));
  assert(decide(12345, 0xF2, before, after).mask == changed(before, after));
}

int main() {
  test_baselines_and_lengths();
  test_rpm();
  test_coolant();
  test_proprietary_and_rapid_reversal();
  std::puts("capture_policy: all tests passed");
}
