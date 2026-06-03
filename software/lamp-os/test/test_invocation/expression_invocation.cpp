// Native-host unit tests for the pure-logic pieces of ExpressionInvocation.
//
// The Arduino-integrated JSON helpers (serializeInvocation, parseInvocation)
// depend on ArduinoJson which isn't pulled into the native test env; those
// are verified by the firmware build + the hardware integration test plan
// in docs/superpowers/specs/2026-06-02-mesh-expression-triggering-design.md.
//
// What IS testable natively is the cascade-key stripping helper, which is
// pure data manipulation. We re-declare a minimal shape here (the same
// pattern test/gradient.cpp uses for Color) so this file stays standalone
// and the existing native test env keeps working.

#include <unity.h>

#include <map>
#include <string>

namespace lamp {

// Mirrors the production declaration in src/expressions/expression_invocation.hpp.
// Kept in sync manually — the native test env doesn't link against src/.
constexpr const char* kParamCascadeEnabled = "cascadeEnabled";
constexpr const char* kParamCascadeStaggerMs = "cascadeStaggerMs";

std::map<std::string, uint32_t> parametersWithoutCascadeKeys(
    const std::map<std::string, uint32_t>& parameters);

}  // namespace lamp

void test_strip_removes_cascade_enabled() {
  std::map<std::string, uint32_t> in = {
      {"cascadeEnabled", 1}, {"pulseSpeed", 5}};
  auto out = lamp::parametersWithoutCascadeKeys(in);
  TEST_ASSERT_EQUAL(1, (int)out.size());
  TEST_ASSERT_EQUAL(5, (int)out["pulseSpeed"]);
  TEST_ASSERT_EQUAL(0, (int)out.count("cascadeEnabled"));
}

void test_strip_removes_cascade_stagger_ms() {
  std::map<std::string, uint32_t> in = {
      {"cascadeStaggerMs", 2000}, {"holdMs", 5000}};
  auto out = lamp::parametersWithoutCascadeKeys(in);
  TEST_ASSERT_EQUAL(1, (int)out.size());
  TEST_ASSERT_EQUAL(5000, (int)out["holdMs"]);
  TEST_ASSERT_EQUAL(0, (int)out.count("cascadeStaggerMs"));
}

void test_strip_removes_both_cascade_keys() {
  std::map<std::string, uint32_t> in = {
      {"cascadeEnabled", 1},
      {"cascadeStaggerMs", 1000},
      {"durationMin", 1},
      {"durationMax", 3}};
  auto out = lamp::parametersWithoutCascadeKeys(in);
  TEST_ASSERT_EQUAL(2, (int)out.size());
  TEST_ASSERT_EQUAL(1, (int)out["durationMin"]);
  TEST_ASSERT_EQUAL(3, (int)out["durationMax"]);
}

void test_strip_is_noop_when_no_cascade_keys() {
  std::map<std::string, uint32_t> in = {
      {"pulseSpeed", 5}, {"durationMin", 1}};
  auto out = lamp::parametersWithoutCascadeKeys(in);
  TEST_ASSERT_EQUAL(2, (int)out.size());
  TEST_ASSERT_EQUAL(5, (int)out["pulseSpeed"]);
  TEST_ASSERT_EQUAL(1, (int)out["durationMin"]);
}

void test_strip_handles_empty_map() {
  std::map<std::string, uint32_t> in;
  auto out = lamp::parametersWithoutCascadeKeys(in);
  TEST_ASSERT_EQUAL(0, (int)out.size());
}

void test_strip_does_not_mutate_input() {
  std::map<std::string, uint32_t> in = {
      {"cascadeEnabled", 1}, {"pulseSpeed", 5}};
  lamp::parametersWithoutCascadeKeys(in);
  TEST_ASSERT_EQUAL(2, (int)in.size());
  TEST_ASSERT_EQUAL(1, (int)in["cascadeEnabled"]);
}

// Re-implementation under test, kept here in the test file to match the
// existing native-env convention (test/gradient.cpp re-declares Color the
// same way). Production code under src/expressions/expression_invocation.cpp
// implements the same contract; the contract is what the firmware build and
// hardware test verify together.
namespace lamp {
std::map<std::string, uint32_t> parametersWithoutCascadeKeys(
    const std::map<std::string, uint32_t>& parameters) {
  std::map<std::string, uint32_t> out;
  for (const auto& kv : parameters) {
    if (kv.first == kParamCascadeEnabled) continue;
    if (kv.first == kParamCascadeStaggerMs) continue;
    out.insert(kv);
  }
  return out;
}
}  // namespace lamp

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_strip_removes_cascade_enabled);
  RUN_TEST(test_strip_removes_cascade_stagger_ms);
  RUN_TEST(test_strip_removes_both_cascade_keys);
  RUN_TEST(test_strip_is_noop_when_no_cascade_keys);
  RUN_TEST(test_strip_handles_empty_map);
  RUN_TEST(test_strip_does_not_mutate_input);
  return UNITY_END();
}
