#include "./standard_lamp.hpp"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Preferences.h>

#include <cstdint>
#include <string>

#include "../components/network/bluetooth.hpp"
#include "../components/network/wifi.hpp"
#include "../expressions/expression_manager.hpp"
#include "../util/color.hpp"
#include "./behaviors/configurator.hpp"
#include "./behaviors/dmx.hpp"
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

// ── Cross-task action queue ─────────────────────────────────────────────────
//
// BLE GATT callbacks run on the NimBLE host task (Core 0). The render loop and
// compositor.tick() run on the Arduino loop task (Core 1). They share LED
// state (NeoPixel brightness, ConfiguratorBehavior colors, knockout pixels).
// Mutating these from the BLE task while loop() reads them is a real data
// race: vector reassignment under iteration crashes mid-frame.
//
// Solution: BLE callbacks deposit pending actions into these slots. Loop()
// drains them at the top of each tick BEFORE rendering. Single-slot last-write-
// wins is fine for slider-drag UX. Mutex-protected because vector copy isn't
// atomic on dual-core ESP32.

constexpr uint8_t MAX_PENDING_COLORS = 5;

struct PendingColorUpdate {
  bool valid = false;
  uint8_t count = 0;
  lamp::Color colors[MAX_PENDING_COLORS];
};

volatile int8_t pendingBrightness = -1;       // -1 = no pending change
PendingColorUpdate pendingBaseColors;
PendingColorUpdate pendingShadeColors;
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// Called from BLE callbacks (Core 0). Stores up to MAX_PENDING_COLORS colors
// in the pending slot for the loop to apply.
static void postPendingColors(PendingColorUpdate& slot, const std::vector<lamp::Color>& newColors) {
  portENTER_CRITICAL(&pendingMux);
  slot.count = newColors.size() < MAX_PENDING_COLORS ? newColors.size() : MAX_PENDING_COLORS;
  for (uint8_t i = 0; i < slot.count; i++) slot.colors[i] = newColors[i];
  slot.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

void postPendingBaseColors(const std::vector<lamp::Color>& c) { postPendingColors(pendingBaseColors, c); }
void postPendingShadeColors(const std::vector<lamp::Color>& c) { postPendingColors(pendingShadeColors, c); }
void postPendingBrightness(int8_t level) { pendingBrightness = level; }
uint32_t lastStageModeCheckTimeMs = 0;
uint32_t lastDmxCheckTimeMs = 0;
uint32_t lastArtnetFrameTimeMs = 0;
lamp::ArtnetDetail artnetData;
lamp::BluetoothComponent bt;
lamp::WifiComponent wifi;
lamp::Compositor compositor;
lamp::FrameBuffer shade;
lamp::FrameBuffer base;
lamp::DmxBehavior shadeDmxBehavior;
lamp::DmxBehavior baseDmxBehavior;
lamp::SocialBehavior shadeSocialBehavior;
lamp::ConfiguratorBehavior shadeConfiguratorBehavior;
lamp::ConfiguratorBehavior baseConfiguratorBehavior;
lamp::FadeOutBehavior shadeFadeOutBehavior;
lamp::FadeOutBehavior baseFadeOutBehavior;
lamp::KnockoutBehavior baseKnockoutBehavior;
lamp::ExpressionManager expressionManager;
lamp::Config config;
unsigned long lastHomeModeUpdateMs = 0;
bool lastHomeMode = false;  // Track previous home mode state

/**
 * Calculate effective home mode based on configuration and network presence
 */
bool calculateEffectiveHomeMode(lamp::Config& config) {
  if (!config.lamp.homeMode) return false;            // Mode disabled
  if (config.lamp.homeModeSSID.empty()) return true;  // No SSID = always home
  return wifi.isHomeNetworkVisible();                 // Check if home network visible
}

void initBehaviors() {
  shadeDmxBehavior = lamp::DmxBehavior(&shade, 480);
  baseDmxBehavior = lamp::DmxBehavior(&base, 480);
  shadeSocialBehavior = lamp::SocialBehavior(&shade, 1200);
  shadeSocialBehavior.setBluetoothComponent(&bt);
  shadeSocialBehavior.allowedInHomeMode = false;
  shadeConfiguratorBehavior = lamp::ConfiguratorBehavior(&shade, 120);
  shadeConfiguratorBehavior.colors = shade.defaultColors;
  baseConfiguratorBehavior = lamp::ConfiguratorBehavior(&base, 120);
  baseConfiguratorBehavior.colors = base.defaultColors;
  shadeFadeOutBehavior = lamp::FadeOutBehavior(&shade, REBOOT_ANIMATION_FRAMES);
  shadeFadeOutBehavior.setWifiComponent(&wifi);
  baseFadeOutBehavior = lamp::FadeOutBehavior(&base, REBOOT_ANIMATION_FRAMES);
  baseFadeOutBehavior.setWifiComponent(&wifi);
  baseKnockoutBehavior = lamp::KnockoutBehavior(&base, 0, true);
  baseKnockoutBehavior.knockoutPixels = config.base.knockoutPixels;

  // Initialize expressions
  expressionManager.begin(&shade, &base);
  expressionManager.loadFromConfig(config.expressions);

  // Build behavior vector in priority order (lowest to highest)
  // Behaviors run in sequence, so later ones override earlier ones
  std::vector<lamp::AnimatedBehavior*> allBehaviors = {};

  // Add expression behaviors (lowest priority - automated effects)
  auto exprBehaviors = expressionManager.getBehaviors();
  allBehaviors.insert(allBehaviors.end(), exprBehaviors.begin(), exprBehaviors.end());

  // Add DMX behaviors (middle priority - live control)
  allBehaviors.push_back(&baseDmxBehavior);
  allBehaviors.push_back(&shadeDmxBehavior);

  // Add social greeting behaviors (high priority)
  allBehaviors.push_back(&shadeSocialBehavior);

  // Add configurator behaviors (highest priority - UI preview, overrides DMX and social)
  allBehaviors.push_back(&baseConfiguratorBehavior);
  allBehaviors.push_back(&shadeConfiguratorBehavior);

  // Add fade behaviors (always last - startup/shutdown effects)
  allBehaviors.push_back(&baseFadeOutBehavior);
  allBehaviors.push_back(&shadeFadeOutBehavior);

  // layers load in priority sequence {lowest, ..., highest}
  compositor.begin(allBehaviors, {&shade, &base}, calculateEffectiveHomeMode(config));

  // Add overlay behaviors
  compositor.overlayBehaviors.push_back(&baseKnockoutBehavior);

  // Set global compositor for expressions
  lamp::setGlobalCompositor(&compositor);

  // Set global expression manager for inter-expression communication
  lamp::setGlobalExpressionManager(&expressionManager);
}

void handleArtnet() {
  uint32_t now = millis();

  if (now > lastDmxCheckTimeMs + 2) {
    lastDmxCheckTimeMs = now;
    lastArtnetFrameTimeMs = wifi.getLastArtnetFrameTimeMs();
    artnetData = wifi.getArtnetData();

    shadeDmxBehavior.setColor(artnetData.shadeColor);
    shadeDmxBehavior.setLastArtnetFrameTimeMs(lastArtnetFrameTimeMs);
    baseDmxBehavior.setColor(artnetData.baseColor);
    baseDmxBehavior.setLastArtnetFrameTimeMs(lastArtnetFrameTimeMs);
  }
};

void handleStageMode() {
  uint32_t now = millis();

  if (now > lastStageModeCheckTimeMs + 2000) {
    lastStageModeCheckTimeMs = now;
    auto foundStages = bt.getStages();

    if (wifi.stageMode && foundStages->size() == 0) {
      wifi.toApMode();
    } else if (!wifi.stageMode && foundStages->size() > 0) {
      wifi.toStageMode(foundStages->at(0).ssid, foundStages->at(0).password);
    }
  }
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

#ifdef LAMP_DEBUG
  Serial.printf("Parsing expression: type=%s, enabled=%d, intervalMin=%lu, intervalMax=%lu\n",
                expr.type.c_str(), expr.enabled, expr.intervalMin, expr.intervalMax);
#endif

  // Parse colors
  JsonArray colors = node["colors"];
  if (colors.size()) {
    for (JsonVariant color : colors) {
      expr.colors.push_back(lamp::hexStringToColor(color));
    }
  }

  // Parse generic parameters - store any additional fields as parameters
  for (JsonPair kv : node) {
    const char* key = kv.key().c_str();
    std::string keyStr(key);

    // Skip common fields we've already handled
    if (keyStr == "type" || keyStr == "enabled" || keyStr == "intervalMin" ||
        keyStr == "intervalMax" || keyStr == "target" || keyStr == "colors") {
      continue;
    }

    // Store the parameter value
    JsonVariant value = kv.value();
    if (value.is<uint32_t>()) {
      expr.setParameter(keyStr, value.as<uint32_t>());
    } else if (value.is<int>()) {
      expr.setParameter(keyStr, static_cast<uint32_t>(value.as<int>()));
    }

#ifdef LAMP_DEBUG
    Serial.printf("Stored parameter: %s\n", keyStr.c_str());
#endif
  }

  return expr;
}

void dispatchLampAction(JsonDocument& doc, unsigned long updateTimeMs) {
  shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = updateTimeMs;
  baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = updateTimeMs;

  // parse the action id (a) into a String
  String action = String(doc["a"]);
  if (action == "bright") {
    int level = doc["v"] | 100;
    // Defer to loop — see PendingColorUpdate notes above. Direct setBrightness
    // from BLE task races with compositor.tick() rendering on Core 1.
    config.lamp.brightness = level;
    postPendingBrightness(static_cast<int8_t>(level & 0x7F));
  } else if (action == "knockout") {
    int pixelIndex = doc["p"];
    int percentage = doc["b"];
    if (pixelIndex >= 0 && pixelIndex < 50 && percentage >= 0 && percentage <= 100) {
      baseKnockoutBehavior.knockoutPixels[pixelIndex] = percentage;
      config.base.knockoutPixels[pixelIndex] = percentage;
    }
  } else if (action == "base") {
    JsonArray baseColors = doc["c"];
    if (baseColors.size()) {
      std::vector<lamp::Color> updatedColors;
      for (JsonVariant baseColor : baseColors) {
        updatedColors.push_back(lamp::hexStringToColor(baseColor));
      }
      // Defer the buildGradient + assignment to loop — vector reassignment
      // here would race with compositor.tick() reading the configurator's
      // colors on the other core.
      postPendingBaseColors(updatedColors);
    }
  } else if (action == "shade") {
    JsonArray shadeColors = doc["c"];
    if (shadeColors.size()) {
      std::vector<lamp::Color> updatedColors;
      for (JsonVariant shadeColor : shadeColors) {
        updatedColors.push_back(lamp::hexStringToColor(shadeColor));
      }
      postPendingShadeColors(updatedColors);
    }
  } else if (action == "test_expression") {
    String type = String(doc["type"]);
    if (type.length() > 0) {
#ifdef LAMP_DEBUG
      Serial.printf("Testing expression: %s\n", type.c_str());
#endif
      // Disable configurator during expression test so expression shows against actual base colors
      shadeConfiguratorBehavior.disabled = true;
      baseConfiguratorBehavior.disabled = true;
      expressionManager.triggerExpression(type.c_str());
    }
  } else if (action == "test_expression_complete") {
    // Re-enable configurator after test expression completes
    shadeConfiguratorBehavior.disabled = false;
    baseConfiguratorBehavior.disabled = false;
    // Reset timer to keep preview active
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();

    // Restore preview colors
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

void handleWebSocket() {
  if (wifi.hasWebSocketData()) {
    JsonDocument doc = wifi.getWebSocketData();
    dispatchLampAction(doc, wifi.getLastWebSocketUpdateTimeMs());
  }
}

void setup() {
#ifdef LAMP_DEBUG
  Serial.begin(115200);
#endif
  config = lamp::Config(&prefs);
  SPIFFS.begin(true);
  bt.begin(config.lamp.name, config.base.colors[config.base.ac], config.shade.colors[0]);
  wifi.begin(&config);
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
  // Drain pending BLE actions BEFORE the render. All mutations of behavior
  // state and strip brightness must happen on this task to avoid racing the
  // compositor / NeoPixel show() on Core 1.

  if (pendingBrightness >= 0) {
    uint8_t level = static_cast<uint8_t>(pendingBrightness);
    if (shadeStrip) shadeStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
    if (baseStrip)  baseStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
    // setBrightness scales the strip's pixel buffer IN PLACE with lossy integer
    // math. When the configurator is in PAUSED state (idle after the ease-in),
    // it isn't writing fresh pixels, so the scaled values stick — colors look
    // damaged or "reset." Force the configurators to redraw at the new
    // brightness so each frame writes fresh, un-scaled values.
    shadeConfiguratorBehavior.playOnce();
    baseConfiguratorBehavior.playOnce();
    pendingBrightness = -1;
  }

  // Drain pending color updates. We must NOT allocate memory inside the
  // critical section (push_back can call malloc, which on ESP32 can deadlock
  // with interrupts disabled). Copy into a stack array first, exit the
  // critical section, then build the std::vector outside.
  if (pendingBaseColors.valid) {
    lamp::Color tmp[MAX_PENDING_COLORS];
    uint8_t count;
    portENTER_CRITICAL(&pendingMux);
    count = pendingBaseColors.count;
    for (uint8_t i = 0; i < count; i++) tmp[i] = pendingBaseColors.colors[i];
    pendingBaseColors.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    std::vector<lamp::Color> colors(tmp, tmp + count);
    baseConfiguratorBehavior.colors = lamp::buildGradientWithStops(base.pixelCount, colors);
  }

  if (pendingShadeColors.valid) {
    lamp::Color tmp[MAX_PENDING_COLORS];
    uint8_t count;
    portENTER_CRITICAL(&pendingMux);
    count = pendingShadeColors.count;
    for (uint8_t i = 0; i < count; i++) tmp[i] = pendingShadeColors.colors[i];
    pendingShadeColors.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    std::vector<lamp::Color> colors(tmp, tmp + count);
    shadeConfiguratorBehavior.colors = lamp::buildGradientWithStops(shade.pixelCount, colors);
  }

  handleStageMode();
  handleArtnet();
  handleWebSocket();
  wifi.tick();

  // Update compositor home mode state periodically for social behaviors
  static constexpr uint32_t HOME_MODE_UPDATE_INTERVAL_MS = 30000;
  if (millis() - lastHomeModeUpdateMs > HOME_MODE_UPDATE_INTERVAL_MS) {
    bool effectiveHomeMode = calculateEffectiveHomeMode(config);
    compositor.setHomeMode(effectiveHomeMode);

    // Apply brightness when home mode state changes
    if (effectiveHomeMode != lastHomeMode) {
      uint8_t targetBrightness = effectiveHomeMode ? config.lamp.homeModeBrightness : config.lamp.brightness;

      shadeStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, targetBrightness));
      baseStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, targetBrightness));

      lastHomeMode = effectiveHomeMode;
    }

    lastHomeModeUpdateMs = millis();
  }

  compositor.tick();
};
