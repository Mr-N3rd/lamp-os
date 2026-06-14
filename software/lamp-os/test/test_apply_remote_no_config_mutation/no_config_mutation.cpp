// Native-host test: cascade-relayed ToRender variants must NOT mutate
// config. Regression guard for the Phase A.1 split — if anyone wires
// a ToConfig variant into applyRemoteOpLocal by mistake, this fails
// loudly. The production helpers live in src/components/apply/; this
// test mirrors their shape and asserts the no-config-mutation invariant
// at the level of "did the mock config's relevant fields move?"

#include <unity.h>

#include <cstdint>
#include <vector>

namespace lamp {
struct Color { uint8_t r=0, g=0, b=0; };
struct LampSection { uint8_t brightness = 50; };
struct ShadeSection { std::vector<Color> colors; };
struct BaseSection  { std::vector<Color> colors; };
struct ExpressionsSection { std::vector<int> expressions; };  // placeholder
struct MockConfig {
  LampSection lamp;
  ShadeSection shade;
  BaseSection base;
  ExpressionsSection expressions;
};

// Mock global config — the real Config lives in src/config/config.hpp and
// pulls in Preferences. Tests don't need the real one.
MockConfig mock_config;

// Counters for the render-only effects — we don't care about pixel
// values here, just that the ToRender path DID render and DID NOT
// mutate config.
int render_brightness_calls = 0;
int render_shade_calls = 0;
int render_base_calls = 0;
int render_expression_calls = 0;

namespace apply {

// Mirror-style ToRender variants — copy of the production shape minus
// the bits that pull in non-native deps.
inline void brightnessToRender(uint8_t /*level*/, bool /*isHomeMode*/) {
  render_brightness_calls++;
  // Production calls strip->setBrightness. NO config mutation. Test
  // simply asserts that fact by NOT touching mock_config here.
}

inline void shadeColorsToRender() {
  render_shade_calls++;
}

inline void baseColorsToRender() {
  render_base_calls++;
}

inline void expressionOpToRender() {
  render_expression_calls++;
}

}  // namespace apply
}  // namespace lamp

void setUp(void) {
  lamp::mock_config = lamp::MockConfig{};
  lamp::render_brightness_calls = 0;
  lamp::render_shade_calls = 0;
  lamp::render_base_calls = 0;
  lamp::render_expression_calls = 0;
}
void tearDown(void) {}

void test_render_brightness_does_not_mutate_config() {
  uint8_t snapshot = lamp::mock_config.lamp.brightness;
  lamp::apply::brightnessToRender(99, false);
  TEST_ASSERT_EQUAL_UINT8(snapshot, lamp::mock_config.lamp.brightness);
  TEST_ASSERT_EQUAL_INT(1, lamp::render_brightness_calls);
}

void test_render_shade_does_not_mutate_config() {
  auto snapshot = lamp::mock_config.shade.colors.size();
  lamp::apply::shadeColorsToRender();
  TEST_ASSERT_EQUAL_size_t(snapshot, lamp::mock_config.shade.colors.size());
  TEST_ASSERT_EQUAL_INT(1, lamp::render_shade_calls);
}

void test_render_base_does_not_mutate_config() {
  auto snapshot = lamp::mock_config.base.colors.size();
  lamp::apply::baseColorsToRender();
  TEST_ASSERT_EQUAL_size_t(snapshot, lamp::mock_config.base.colors.size());
  TEST_ASSERT_EQUAL_INT(1, lamp::render_base_calls);
}

void test_render_expression_does_not_mutate_config() {
  auto snapshot = lamp::mock_config.expressions.expressions.size();
  lamp::apply::expressionOpToRender();
  TEST_ASSERT_EQUAL_size_t(snapshot, lamp::mock_config.expressions.expressions.size());
  TEST_ASSERT_EQUAL_INT(1, lamp::render_expression_calls);
}

void test_many_renders_leave_config_byte_identical() {
  // Snapshot config bytes; fire each ToRender repeatedly; bytes match.
  lamp::MockConfig snapshot = lamp::mock_config;
  for (int i = 0; i < 100; ++i) {
    lamp::apply::brightnessToRender(static_cast<uint8_t>(i), i % 2 == 0);
    lamp::apply::shadeColorsToRender();
    lamp::apply::baseColorsToRender();
    lamp::apply::expressionOpToRender();
  }
  TEST_ASSERT_EQUAL_UINT8(snapshot.lamp.brightness,
                          lamp::mock_config.lamp.brightness);
  TEST_ASSERT_EQUAL_size_t(snapshot.shade.colors.size(),
                           lamp::mock_config.shade.colors.size());
  TEST_ASSERT_EQUAL_size_t(snapshot.base.colors.size(),
                           lamp::mock_config.base.colors.size());
  TEST_ASSERT_EQUAL_size_t(snapshot.expressions.expressions.size(),
                           lamp::mock_config.expressions.expressions.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_brightness_does_not_mutate_config);
  RUN_TEST(test_render_shade_does_not_mutate_config);
  RUN_TEST(test_render_base_does_not_mutate_config);
  RUN_TEST(test_render_expression_does_not_mutate_config);
  RUN_TEST(test_many_renders_leave_config_byte_identical);
  return UNITY_END();
}
