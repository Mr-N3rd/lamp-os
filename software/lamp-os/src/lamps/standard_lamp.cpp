#include "./standard_lamp.hpp"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Preferences.h>

#include <cstdint>
#include <string>

#include "../components/network/bluetooth.hpp"
#include "../expressions/expression_manager.hpp"
#include "../util/color.hpp"
#include "./behaviors/configurator.hpp"
#include "./behaviors/fade_out.hpp"
#include "./behaviors/knockout.hpp"
#include "./behaviors/social.hpp"
#include "./config/config.hpp"
#include "./core/animated_behavior.hpp"
#include "./core/compositor.hpp"
#include "./core/frame_buffer.hpp"
#include "./globals.hpp"
#include "./util/color.hpp"
#include "./util/gradient.hpp"
#include "./util/levels.hpp"
#include "SPIFFS.h"

Adafruit_NeoPixel* shadeStrip = nullptr;
Adafruit_NeoPixel* baseStrip = nullptr;
Preferences prefs;

// Zero-allocation pending slots. BLE callbacks on the NimBLE host task
// (Core 0) only do a fixed-size memcpy under portMUX into these slots. The
// loop task (Core 1) drains them and does ALL heap work — JSON parsing,
// vector building, state mutation. Single-core memory pattern.

constexpr size_t MAX_PENDING_JSON = 256;

struct PendingJsonUpdate {
  bool valid = false;
  uint16_t length = 0;
  char json[MAX_PENDING_JSON];
};

struct PendingKnockoutUpdate {
  bool valid = false;
  uint8_t pixel = 0;
  uint8_t brightness = 100;
};

volatile int8_t pendingBrightness = -1;
PendingJsonUpdate pendingBaseColorsJson;
PendingJsonUpdate pendingShadeColorsJson;
PendingKnockoutUpdate pendingKnockout;
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

static void postPendingJson(PendingJsonUpdate& slot, const char* data, size_t len) {
  if (len > MAX_PENDING_JSON) return;
  portENTER_CRITICAL(&pendingMux);
  slot.length = static_cast<uint16_t>(len);
  memcpy(slot.json, data, len);
  slot.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

void postPendingShadeColorsJson(const char* data, size_t len) { postPendingJson(pendingShadeColorsJson, data, len); }
void postPendingBaseColorsJson(const char* data, size_t len)  { postPendingJson(pendingBaseColorsJson, data, len); }
void postPendingBrightness(int8_t level) { pendingBrightness = level; }
void postPendingKnockout(uint8_t pixel, uint8_t brightness) {
  portENTER_CRITICAL(&pendingMux);
  pendingKnockout.pixel = pixel;
  pendingKnockout.brightness = brightness;
  pendingKnockout.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

lamp::BluetoothComponent bt;
lamp::Compositor compositor;
lamp::FrameBuffer shade;
lamp::FrameBuffer base;
lamp::SocialBehavior shadeSocialBehavior;
lamp::ConfiguratorBehavior shadeConfiguratorBehavior;
lamp::ConfiguratorBehavior baseConfiguratorBehavior;
lamp::FadeOutBehavior shadeFadeOutBehavior;
lamp::FadeOutBehavior baseFadeOutBehavior;
lamp::KnockoutBehavior baseKnockoutBehavior;
lamp::ExpressionManager expressionManager;
lamp::Config config;

void initBehaviors() {
  shadeSocialBehavior = lamp::SocialBehavior(&shade, 1200);
  shadeSocialBehavior.setBluetoothComponent(&bt);
  shadeConfiguratorBehavior = lamp::ConfiguratorBehavior(&shade, 120);
  shadeConfiguratorBehavior.colors = shade.defaultColors;
  baseConfiguratorBehavior = lamp::ConfiguratorBehavior(&base, 120);
  baseConfiguratorBehavior.colors = base.defaultColors;
  shadeFadeOutBehavior = lamp::FadeOutBehavior(&shade, REBOOT_ANIMATION_FRAMES);
  baseFadeOutBehavior = lamp::FadeOutBehavior(&base, REBOOT_ANIMATION_FRAMES);
  baseKnockoutBehavior = lamp::KnockoutBehavior(&base, 0, true);
  baseKnockoutBehavior.knockoutPixels = config.base.knockoutPixels;

  expressionManager.begin(&shade, &base);
  expressionManager.loadFromConfig(config.expressions);

  std::vector<lamp::AnimatedBehavior*> allBehaviors = {};

  // Expression behaviors (lowest priority — automated effects)
  auto exprBehaviors = expressionManager.getBehaviors();
  allBehaviors.insert(allBehaviors.end(), exprBehaviors.begin(), exprBehaviors.end());

  // Social greeting behaviors (high priority)
  allBehaviors.push_back(&shadeSocialBehavior);

  // Configurator behaviors (highest priority — UI preview)
  allBehaviors.push_back(&baseConfiguratorBehavior);
  allBehaviors.push_back(&shadeConfiguratorBehavior);

  // Fade-out behaviors run last so reboot animation is on top of everything
  allBehaviors.push_back(&baseFadeOutBehavior);
  allBehaviors.push_back(&shadeFadeOutBehavior);

  compositor.begin(allBehaviors, {&shade, &base}, /*homeMode=*/false);
  compositor.overlayBehaviors.push_back(&baseKnockoutBehavior);

  lamp::setGlobalCompositor(&compositor);
  lamp::setGlobalExpressionManager(&expressionManager);
}

/**
 * Parse ExpressionConfig from JSON object using generic parameter system
 */
lamp::ExpressionConfig parseExpressionConfig(JsonObject node) {
  lamp::ExpressionConfig expr;
  expr.type = std::string(node["type"] | "");
  expr.enabled = node["enabled"] | false;
  expr.intervalMin = node["intervalMin"] | 60;
  expr.intervalMax = node["intervalMax"] | 900;
  expr.target = node["target"] | 3;

  JsonArray colors = node["colors"];
  if (colors.size()) {
    for (JsonVariant color : colors) {
      expr.colors.push_back(lamp::hexStringToColor(color));
    }
  }

  for (JsonPair kv : node) {
    const char* key = kv.key().c_str();
    std::string keyStr(key);
    if (keyStr == "type" || keyStr == "enabled" || keyStr == "intervalMin" ||
        keyStr == "intervalMax" || keyStr == "target" || keyStr == "colors") {
      continue;
    }
    JsonVariant value = kv.value();
    if (value.is<uint32_t>()) {
      expr.setParameter(keyStr, value.as<uint32_t>());
    } else if (value.is<int>()) {
      expr.setParameter(keyStr, static_cast<uint32_t>(value.as<int>()));
    }
  }

  return expr;
}

void dispatchLampAction(JsonDocument& doc, unsigned long updateTimeMs) {
  shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = updateTimeMs;
  baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = updateTimeMs;

  String action = String(doc["a"]);
  if (action == "test_expression") {
    String type = String(doc["type"]);
    if (type.length() > 0) {
      // If the payload carries colors/interval, reconfigure the existing
      // Expression instances in place so Test reflects the UI's current
      // edits without a settings save. We can NOT addExpression() at runtime
      // because the compositor's behavior list is built once at setup.
      if (doc["colors"].is<JsonArray>()) {
        std::vector<lamp::Color> newColors;
        for (JsonVariant cv : doc["colors"].as<JsonArray>()) {
          newColors.push_back(lamp::hexStringToColor(cv));
        }
        uint32_t intervalMin = doc["intervalMin"] | 60;
        uint32_t intervalMax = doc["intervalMax"] | 900;
        lamp::ExpressionTarget target = doc["target"].is<int>()
          ? static_cast<lamp::ExpressionTarget>(doc["target"].as<int>())
          : lamp::TARGET_BOTH;
        expressionManager.reconfigureByType(type.c_str(), newColors, intervalMin, intervalMax, target);
      }
#ifdef LAMP_DEBUG
      auto colors = expressionManager.getExpressionColors(type.c_str());
      String colorList;
      for (const auto& c : colors) {
        if (colorList.length() > 0) colorList += " ";
        colorList += lamp::colorToHexString(c).c_str();
      }
      Serial.printf("Testing expression: %s [%s]\n", type.c_str(), colorList.c_str());
#endif
      shadeConfiguratorBehavior.disabled = true;
      baseConfiguratorBehavior.disabled = true;
      expressionManager.triggerExpression(type.c_str());
    }
  } else if (action == "test_expression_complete") {
    shadeConfiguratorBehavior.disabled = false;
    baseConfiguratorBehavior.disabled = false;
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();

    if (doc["shadeColors"]) {
      JsonArray shadeColors = doc["shadeColors"];
      if (shadeColors.size()) {
        std::vector<lamp::Color> updatedColors;
        for (JsonVariant shadeColor : shadeColors) {
          updatedColors.push_back(lamp::hexStringToColor(shadeColor));
        }
        shadeConfiguratorBehavior.colors = lamp::buildGradientWithStops(shade.pixelCount, updatedColors);
      }
    }
    if (doc["baseColors"]) {
      JsonArray baseColors = doc["baseColors"];
      if (baseColors.size()) {
        std::vector<lamp::Color> updatedColors;
        for (JsonVariant baseColor : baseColors) {
          updatedColors.push_back(lamp::hexStringToColor(baseColor));
        }
        baseConfiguratorBehavior.colors = lamp::buildGradientWithStops(base.pixelCount, updatedColors);
      }
    }
  }
}

extern void lamp_register_panic_handler();

void setup() {
#ifdef LAMP_DEBUG
  Serial.begin(115200);
#endif
  lamp_register_panic_handler();
  config = lamp::Config(&prefs);
  SPIFFS.begin(true);

  bt.begin(config.lamp.name, config.base.colors[config.base.ac], config.shade.colors[0]);
  bt.activateGattServices(&config, &prefs);

  const uint16_t shadeFmt = (config.shade.bpp == 3) ? NEO_GRB : NEO_GRBW;
  const uint16_t baseFmt = (config.base.bpp == 3) ? NEO_GRB : NEO_GRBW;
  shadeStrip = new Adafruit_NeoPixel(LAMP_MAX_STRIP_PIXELS_SHADE, LAMP_SHADE_PIN, shadeFmt + NEO_KHZ800);
  baseStrip = new Adafruit_NeoPixel(LAMP_MAX_STRIP_PIXELS_BASE, LAMP_BASE_PIN, baseFmt + NEO_KHZ800);
  shadeStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, config.lamp.brightness));
  baseStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, config.lamp.brightness));
  shade.begin(lamp::buildGradientWithStops(config.shade.px, config.shade.colors), config.shade.px, shadeStrip);
  base.begin(lamp::buildGradientWithStops(config.base.px, config.base.colors), config.base.px, baseStrip);
  initBehaviors();
};

void loop() {
  // Drain pending BLE actions on the loop task (Core 1). All heap allocation
  // (JsonDocument parse, std::vector, gradient construction) happens here,
  // NOT in BLE callbacks on Core 0.

  if (pendingBrightness >= 0) {
    uint8_t level = static_cast<uint8_t>(pendingBrightness);
    pendingBrightness = -1;
    config.lamp.brightness = level;
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    if (shadeStrip) shadeStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
    if (baseStrip)  baseStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
  }

  if (pendingShadeColorsJson.valid) {
    char buf[MAX_PENDING_JSON + 1];
    uint16_t len;
    portENTER_CRITICAL(&pendingMux);
    len = pendingShadeColorsJson.length;
    memcpy(buf, pendingShadeColorsJson.json, len);
    pendingShadeColorsJson.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    buf[len] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      JsonArray arr = doc.as<JsonArray>();
      if (arr.size() > 0) {
        std::vector<lamp::Color> colors;
        colors.reserve(arr.size());
        for (JsonVariant v : arr) {
          colors.push_back(lamp::hexStringToColor(v));
        }
        shadeConfiguratorBehavior.colors = lamp::buildGradientWithStops(shade.pixelCount, colors);
      }
    }
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
  }

  if (pendingBaseColorsJson.valid) {
    char buf[MAX_PENDING_JSON + 1];
    uint16_t len;
    portENTER_CRITICAL(&pendingMux);
    len = pendingBaseColorsJson.length;
    memcpy(buf, pendingBaseColorsJson.json, len);
    pendingBaseColorsJson.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    buf[len] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      JsonArray arr = doc.as<JsonArray>();
      if (arr.size() > 0) {
        std::vector<lamp::Color> colors;
        colors.reserve(arr.size());
        for (JsonVariant v : arr) {
          colors.push_back(lamp::hexStringToColor(v));
        }
        baseConfiguratorBehavior.colors = lamp::buildGradientWithStops(base.pixelCount, colors);
      }
    }
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
  }

  if (pendingKnockout.valid) {
    uint8_t pixel, brightness;
    portENTER_CRITICAL(&pendingMux);
    pixel = pendingKnockout.pixel;
    brightness = pendingKnockout.brightness;
    pendingKnockout.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    if (pixel < 50 && brightness <= 100) {
      baseKnockoutBehavior.knockoutPixels[pixel] = brightness;
      config.base.knockoutPixels[pixel] = brightness;
    }
  }

  compositor.tick();
};
