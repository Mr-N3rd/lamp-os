#include "./standard_lamp.hpp"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Preferences.h>

#include <cstdint>
#include <string>

#include "../components/network/bluetooth.hpp"
#include "../components/network/ble_control.hpp"
#include "../components/network/mqtt.hpp"
#include "../components/network/nearby_lamps.hpp"
#include "../components/network/show_receiver.hpp"
#include "../components/network/wifi.hpp"
#include "../behaviors/show_behavior.hpp"
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

Adafruit_NeoPixel* shadeStrip = nullptr;
Adafruit_NeoPixel* baseStrip = nullptr;
Preferences prefs;

// Zero-allocation pending slots. BLE callbacks on the NimBLE host task
// (Core 0) only do a fixed-size memcpy under portMUX into these slots. The
// loop task (Core 1) drains them and does ALL heap work — JSON parsing,
// vector building, state mutation. Single-core memory pattern.

constexpr size_t MAX_PENDING_JSON = 256;
constexpr size_t MAX_PENDING_OP_JSON = 512;  // expression op payloads are larger

struct PendingJsonUpdate {
  bool valid = false;
  uint16_t length = 0;
  char json[MAX_PENDING_JSON];
};

struct PendingOpJsonUpdate {
  bool valid = false;
  uint16_t length = 0;
  char json[MAX_PENDING_OP_JSON];
};

struct PendingKnockoutUpdate {
  bool valid = false;
  uint8_t pixel = 0;
  uint8_t brightness = 100;
};

volatile int8_t pendingBrightness = -1;
// Flag set from Core 0 (BLE callbacks) when the home-mode preview state
// changes — either the flag itself flipped, or homeMode.brightness was
// updated via CHAR_HOME_PREVIEW cmd 0x02. The loop task on Core 1 drains
// it and calls applyEffectiveBrightness so the strip transitions cleanly.
volatile bool pendingApplyEffectiveBrightness = false;
PendingJsonUpdate pendingBaseColorsJson;
PendingJsonUpdate pendingShadeColorsJson;
PendingKnockoutUpdate pendingKnockout;
PendingOpJsonUpdate pendingExpressionOpJson;
PendingOpJsonUpdate pendingWifiOpJson;
PendingOpJsonUpdate pendingTestActionJson;
PendingOpJsonUpdate pendingMqttOpJson;
PendingOpJsonUpdate pendingRemoteOpJson;
// Slot for ESP-NOW-inbound CONTROL_OP payloads. ShowReceiver runs the recv
// callback on the WiFi task; it memcpys the payload here and the loop task
// drains + dispatches. Mirrors the BLE Core 0 → loop pattern.
PendingOpJsonUpdate pendingInboundOpJson;
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
void postPendingApplyEffectiveBrightness() { pendingApplyEffectiveBrightness = true; }
void postPendingKnockout(uint8_t pixel, uint8_t brightness) {
  portENTER_CRITICAL(&pendingMux);
  pendingKnockout.pixel = pixel;
  pendingKnockout.brightness = brightness;
  pendingKnockout.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

void postPendingExpressionOpJson(const char* data, size_t len) {
  if (len > MAX_PENDING_OP_JSON) return;
  portENTER_CRITICAL(&pendingMux);
  pendingExpressionOpJson.length = static_cast<uint16_t>(len);
  memcpy(pendingExpressionOpJson.json, data, len);
  pendingExpressionOpJson.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

void postPendingWifiOpJson(const char* data, size_t len) {
  if (len > MAX_PENDING_OP_JSON) return;
  portENTER_CRITICAL(&pendingMux);
  pendingWifiOpJson.length = static_cast<uint16_t>(len);
  memcpy(pendingWifiOpJson.json, data, len);
  pendingWifiOpJson.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

void postPendingTestActionJson(const char* data, size_t len) {
  if (len > MAX_PENDING_OP_JSON) return;
  portENTER_CRITICAL(&pendingMux);
  pendingTestActionJson.length = static_cast<uint16_t>(len);
  memcpy(pendingTestActionJson.json, data, len);
  pendingTestActionJson.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

void postPendingRemoteOpJson(const char* data, size_t len) {
  if (len > MAX_PENDING_OP_JSON) return;
  portENTER_CRITICAL(&pendingMux);
  pendingRemoteOpJson.length = static_cast<uint16_t>(len);
  memcpy(pendingRemoteOpJson.json, data, len);
  pendingRemoteOpJson.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

void postPendingMqttOpJson(const char* data, size_t len) {
  if (len > MAX_PENDING_OP_JSON) return;
  portENTER_CRITICAL(&pendingMux);
  pendingMqttOpJson.length = static_cast<uint16_t>(len);
  memcpy(pendingMqttOpJson.json, data, len);
  pendingMqttOpJson.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

// Apply a remote-op payload locally (either from BLE remoteOp drain when
// targetMac==self/broadcast, or from an incoming ESP-NOW MSG_CONTROL_OP).
// Parses `char` and re-emits to the matching local pending-slot post. Runs
// on the loop task in both cases (BLE-drain path is direct; ESP-NOW path
// goes via ShowReceiver's WiFi-task handler which only does memcpy into a
// pending slot, then loop drains it — implementation below).
//
// `payload` must be a NUL-terminated JSON string for ArduinoJson to parse.
static void applyRemoteOpLocal(const char* payloadJson, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, payloadJson, len) != DeserializationError::Ok) return;
  const char* ch = doc["char"].as<const char*>();
  if (!ch || !*ch) return;

  if (strcmp(ch, "brightness") == 0) {
    int level = doc["value"] | -1;
    if (level >= 0 && level <= 100) postPendingBrightness(static_cast<int8_t>(level));

  } else if (strcmp(ch, "shadeColors") == 0 || strcmp(ch, "baseColors") == 0) {
    // Both expect a JSON array of hex strings as the payload to their drain.
    // Serialize the `colors` array back out so the existing drain can parse it.
    JsonArray arr = doc["colors"].as<JsonArray>();
    if (arr.isNull()) return;
    std::string colorsJson;
    serializeJson(arr, colorsJson);
    if (strcmp(ch, "shadeColors") == 0) postPendingShadeColorsJson(colorsJson.data(), colorsJson.size());
    else                                 postPendingBaseColorsJson(colorsJson.data(), colorsJson.size());

  } else if (strcmp(ch, "knockout") == 0) {
    int pixel = doc["pixel"] | -1;
    int brightness = doc["brightness"] | -1;
    if (pixel >= 0 && pixel < 256 && brightness >= 0 && brightness <= 100) {
      postPendingKnockout(static_cast<uint8_t>(pixel), static_cast<uint8_t>(brightness));
    }

  } else if (strcmp(ch, "expressionOp") == 0) {
    // Drop the `char` key and forward the rest to the existing expressionOp drain.
    doc.remove("char");
    std::string out;
    serializeJson(doc, out);
    postPendingExpressionOpJson(out.data(), out.size());

  } else if (strcmp(ch, "mqtt") == 0) {
    doc.remove("char");
    std::string out;
    serializeJson(doc, out);
    postPendingMqttOpJson(out.data(), out.size());
  }
  // settings forwarding is intentionally deferred — it triggers a remote
  // reboot whose UX over the grid needs more thought. Follow-up plan.
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
lamp::MqttComponent mqtt;
lamp::ShowReceiver showReceiver;
lamp::ShowBehavior shadeShowBehavior;
lamp::ShowBehavior baseShowBehavior;

void initBehaviors() {
  shadeSocialBehavior = lamp::SocialBehavior(&shade, 1200);
  shadeConfiguratorBehavior = lamp::ConfiguratorBehavior(&shade, 120);
  shadeConfiguratorBehavior.colors = shade.defaultColors;
  baseConfiguratorBehavior = lamp::ConfiguratorBehavior(&base, 120);
  baseConfiguratorBehavior.colors = base.defaultColors;
  shadeFadeOutBehavior = lamp::FadeOutBehavior(&shade, REBOOT_ANIMATION_FRAMES);
  baseFadeOutBehavior = lamp::FadeOutBehavior(&base, REBOOT_ANIMATION_FRAMES);
  baseKnockoutBehavior = lamp::KnockoutBehavior(&base, 0, true);
  baseKnockoutBehavior.knockoutPixels = config.base.knockoutPixels;

  // ShowBehaviors render the latest COLORS frame received via ESP-NOW. They
  // only run while ShowReceiver reports a fresh frame; otherwise they stay
  // STOPPED and lower-priority behaviors (expressions, idle) render.
  shadeShowBehavior = lamp::ShowBehavior(&shade, 0, true);
  shadeShowBehavior.setSide(lamp::ShowBehavior::SHADE);
  shadeShowBehavior.setReceiver(&showReceiver);
  baseShowBehavior = lamp::ShowBehavior(&base, 0, true);
  baseShowBehavior.setSide(lamp::ShowBehavior::BASE);
  baseShowBehavior.setReceiver(&showReceiver);

  expressionManager.begin(&shade, &base);
  expressionManager.loadFromConfig(config.expressions);

  std::vector<lamp::AnimatedBehavior*> allBehaviors = {};

  // Expression behaviors (lowest priority — automated effects)
  auto exprBehaviors = expressionManager.getBehaviors();
  allBehaviors.insert(allBehaviors.end(), exprBehaviors.begin(), exprBehaviors.end());

  // ShowBehaviors sit just above expressions: when a grid COLORS frame is
  // fresh they take over; when it ages out they yield to expressions.
  allBehaviors.push_back(&shadeShowBehavior);
  allBehaviors.push_back(&baseShowBehavior);

  // Social greeting behaviors (high priority)
  allBehaviors.push_back(&shadeSocialBehavior);

  // Configurator behaviors (highest priority — UI preview)
  allBehaviors.push_back(&baseConfiguratorBehavior);
  allBehaviors.push_back(&shadeConfiguratorBehavior);

  // Fade-out behaviors run last so reboot animation is on top of everything
  allBehaviors.push_back(&baseFadeOutBehavior);
  allBehaviors.push_back(&shadeFadeOutBehavior);

  compositor.begin(allBehaviors, {&shade, &base}, /*homeMode=*/false);
  // Record where the initial expression behaviors end so runtime adds insert
  // before higher-priority behaviors (social, configurator, fade-out).
  compositor.setExpressionBandEnd(exprBehaviors.size());
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
      lamp::ExpressionTarget target = doc["target"].is<int>()
        ? static_cast<lamp::ExpressionTarget>(doc["target"].as<int>())
        : lamp::TARGET_BOTH;
#ifdef LAMP_DEBUG
      auto colors = expressionManager.getExpressionColors(type.c_str());
      String colorList;
      for (const auto& c : colors) {
        if (colorList.length() > 0) colorList += " ";
        colorList += lamp::colorToHexString(c).c_str();
      }
      Serial.printf("Testing expression: %s target=%d [%s]\n",
                    type.c_str(), static_cast<int>(target), colorList.c_str());
#endif
      shadeConfiguratorBehavior.disabled = true;
      baseConfiguratorBehavior.disabled = true;
      expressionManager.triggerExpression(type.c_str(), target);
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

// Effective brightness depends on whether we're in home mode. Two ways:
//   1. App-driven preview: the Home Mode setup page is mounted, the app
//      has written CHAR_HOME_PREVIEW cmd 0x01. ble_control holds the flag.
//      Used so the brightness slider on that page has visible effect even
//      though WiFi is paused for the BT session.
//   2. Actual home mode: WiFi STA is associated with the saved home SSID.
// Otherwise falls back to the regular lamp brightness.
static uint8_t effectiveBrightness() {
  if (ble_control::isHomePreviewActive()) {
    return config.homeMode.brightness;
  }
  if (wifi::isConnected() && !config.homeMode.ssid.empty() &&
      wifi::currentSsid() == config.homeMode.ssid) {
    return config.homeMode.brightness;
  }
  return config.lamp.brightness;
}

static void applyEffectiveBrightness() {
  uint8_t level = effectiveBrightness();
  if (shadeStrip) shadeStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
  if (baseStrip) baseStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
}

static void onWifiStateChanged() {
  applyEffectiveBrightness();
  ble_control::notifyWifiState();
}

void setup() {
#ifdef LAMP_DEBUG
  Serial.begin(115200);
#endif
  lamp_register_panic_handler();
  config = lamp::Config(&prefs);

  wifi::begin();
  wifi::setStateChangeCallback(onWifiStateChanged);

  bt.begin(config.lamp.name, config.base.colors[config.base.ac], config.shade.colors[0]);
  bt.activateGattServices(&config, &prefs);

  const uint16_t shadeFmt = (config.shade.bpp == 3) ? NEO_GRB : NEO_GRBW;
  const uint16_t baseFmt = (config.base.bpp == 3) ? NEO_GRB : NEO_GRBW;
  shadeStrip = new Adafruit_NeoPixel(LAMP_MAX_STRIP_PIXELS_SHADE, LAMP_SHADE_PIN, shadeFmt + NEO_KHZ800);
  baseStrip = new Adafruit_NeoPixel(LAMP_MAX_STRIP_PIXELS_BASE, LAMP_BASE_PIN, baseFmt + NEO_KHZ800);
  applyEffectiveBrightness();
  shade.begin(lamp::buildGradientWithStops(config.shade.px, config.shade.colors), config.shade.px, shadeStrip);
  base.begin(lamp::buildGradientWithStops(config.base.px, config.base.colors), config.base.px, baseStrip);
  initBehaviors();

  // Kick off WiFi STA if home mode is configured AND enabled. BLE
  // onConnect/onDisconnect pauses/resumes WiFi for coexistence — this is
  // the boot-time entry. The `enabled` gate lets the user keep stored
  // credentials with home mode soft-toggled off.
  if (config.homeMode.enabled && !config.homeMode.ssid.empty()) {
    wifi::connect(config.homeMode.ssid, config.homeMode.password);
  } else {
    // Not joining a home AP — pin the radio to the grid channel so peers can
    // hear our HELLOs.
    wifi::ensureGridChannel();
  }

  // Bring up ESP-NOW grid presence (HELLO + COLORS). Independent of home
  // WiFi — runs on whatever channel the radio is on. See lamp_protocol.hpp.
  showReceiver.begin(&config);
  // Route inbound CONTROL_OP payloads (addressed to us or broadcast) into a
  // pending slot. WiFi-task safe: pure memcpy under portMUX, no heap work.
  showReceiver.setControlOpHandler([](const uint8_t* payload, size_t len) {
    if (len > MAX_PENDING_OP_JSON) return;
    portENTER_CRITICAL(&pendingMux);
    pendingInboundOpJson.length = static_cast<uint16_t>(len);
    memcpy(pendingInboundOpJson.json, payload, len);
    pendingInboundOpJson.valid = true;
    portEXIT_CRITICAL(&pendingMux);
  });

  // MQTT / Home Assistant integration. No-op unless config.mqtt.enabled and
  // WiFi is up. brightness callback writes the same path the BLE brightness
  // drain uses; power callback toggles to 0 (off) without losing the
  // user's brightness setting.
  mqtt.begin(
      &config,
      // brightness callback (HA slider -> lamp)
      [](uint8_t level) {
        if (level > 100) level = 100;
        config.lamp.brightness = level;
        shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
        baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
        applyEffectiveBrightness();
        ble_control::notifyLampSection();
      },
      // power callback (HA on/off -> lamp)
      [](bool on) {
        uint8_t level = on ? config.lamp.brightness : 0;
        if (shadeStrip) shadeStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
        if (baseStrip)  baseStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
      });
};

void loop() {
  // Drain pending BLE actions on the loop task (Core 1). All heap allocation
  // (JsonDocument parse, std::vector, gradient construction) happens here,
  // NOT in BLE callbacks on Core 0.

  // Reflect mesh state on the manufacturer-data advertisement so the phone
  // app's scanner can light a "mesh" dot beside this lamp.
  //
  // "On mesh" = we've heard at least one other lamp on BLE adv in the last
  // 60 s. BLE-reachable is the most reliable proxy — it's the same signal
  // SocialBehavior uses to greet neighbours and doesn't require Wi-Fi STA
  // to be configured (ESP-NOW peer-to-peer falls back to channel 1 when
  // STA isn't up, which doesn't reliably match other lamps).
  //
  // The setMeshState helper is idempotent: it only re-applies
  // setManufacturerData when the flag flips, so calling it every loop
  // tick is fine — once we're past the post-boot stabilisation window.
  //
  // Skip the first 5 seconds after boot. Otherwise the very first
  // loop tick flips s_meshStateInitialized false→true and rebuilds
  // the advertisement packet via setAdvertisementData, and the first
  // HELLO from a neighbour 1-2 sec later flips meshFlag 0→1 and
  // rebuilds it AGAIN. Both rebuilds momentarily stop + restart
  // advertising on the BLE host task, which destabilises any phone
  // that's mid-connection — the exact failure pattern the AddLamp
  // verify reconnect hits about 5 s post-reboot.
  if (millis() >= 5000) {
    bt.setMeshState(!lamp::nearbyLamps.getReachableViaBle(60000).empty());
  }

  if (pendingApplyEffectiveBrightness) {
    pendingApplyEffectiveBrightness = false;
    applyEffectiveBrightness();
  }

  if (pendingBrightness >= 0) {
    uint8_t level = static_cast<uint8_t>(pendingBrightness);
    pendingBrightness = -1;
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain brightness=%u\n", (unsigned)level);
#endif
    config.lamp.brightness = level;
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    // Apply the brightness the user wrote directly — they're either editing
    // regular brightness, or live-previewing home-mode brightness (WiFi is
    // paused while BLE is connected so effectiveBrightness would resolve to
    // the regular path anyway, but trust the wire value here).
    if (shadeStrip) shadeStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
    if (baseStrip)  baseStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
    // Mirror to HA so the dashboard stays in sync (no-op unless MQTT is
    // enabled, configured, and connected).
    mqtt.publishState();
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

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain shadeColors len=%u\n", (unsigned)len);
#endif

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

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain baseColors len=%u\n", (unsigned)len);
#endif

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
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain knockout pixel=%u brightness=%u\n", pixel, brightness);
#endif
    if (pixel < 50 && brightness <= 100) {
      baseKnockoutBehavior.knockoutPixels[pixel] = brightness;
      config.base.knockoutPixels[pixel] = brightness;
    }
  }

  if (pendingExpressionOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len;
    portENTER_CRITICAL(&pendingMux);
    len = pendingExpressionOpJson.length;
    memcpy(buf, pendingExpressionOpJson.json, len);
    pendingExpressionOpJson.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    buf[len] = '\0';

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain expressionOp len=%u\n", (unsigned)len);
#endif

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      const char* op = doc["op"].as<const char*>();
      if (op && strcmp(op, "upsert") == 0 && doc["entry"].is<JsonObject>()) {
        JsonObject entry = doc["entry"].as<JsonObject>();
        lamp::ExpressionConfig cfg;
        cfg.type = std::string(entry["type"] | "");
        cfg.enabled = entry["enabled"] | true;
        cfg.intervalMin = entry["intervalMin"] | 60;
        cfg.intervalMax = entry["intervalMax"] | 900;
        cfg.target = entry["target"] | 3;
        for (JsonPair kv : entry) {
          std::string key(kv.key().c_str());
          if (key == "type" || key == "enabled" || key == "intervalMin" ||
              key == "intervalMax" || key == "target" || key == "colors") continue;
          JsonVariant v = kv.value();
          if (v.is<uint32_t>()) cfg.setParameter(key, v.as<uint32_t>());
          else if (v.is<int>()) cfg.setParameter(key, static_cast<uint32_t>(v.as<int>()));
        }
        // Store the JsonArray in a local so iteration doesn't reference a
        // temporary that's destroyed at the end of the full expression
        // (ArduinoJson 7.4.x tightened lifetime semantics on chained
        // calls; before this the same code happened to work).
        JsonArray colorsArr = entry["colors"].as<JsonArray>();
        for (JsonVariant cv : colorsArr) {
          cfg.colors.push_back(lamp::hexStringToColor(cv));
        }
        if (!cfg.type.empty()) {
          expressionManager.upsertExpression(cfg, &compositor);
        }
      } else if (op && strcmp(op, "remove") == 0) {
        const char* type = doc["type"].as<const char*>();
        int tgt = doc["target"] | 0;
        if (type && tgt >= 1 && tgt <= 3) {
          expressionManager.removeExpression(type, static_cast<lamp::ExpressionTarget>(tgt), &compositor);
        }
      }
    }
  }

  if (pendingTestActionJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len;
    portENTER_CRITICAL(&pendingMux);
    len = pendingTestActionJson.length;
    memcpy(buf, pendingTestActionJson.json, len);
    pendingTestActionJson.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    buf[len] = '\0';

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain testAction len=%u\n", (unsigned)len);
#endif

    JsonDocument doc;
    if (len == 0) {
      doc["a"] = "test_expression_complete";
      dispatchLampAction(doc, millis());
    } else {
      DeserializationError err = deserializeJson(doc, buf, len);
      const char* action = err ? nullptr : doc["a"].as<const char*>();
      if (action && *action) {
        dispatchLampAction(doc, millis());
      } else {
        doc.clear();
        std::string value(buf, len);
        if (value == "complete") {
          doc["a"] = "test_expression_complete";
        } else {
          doc["a"] = "test_expression";
          doc["type"] = value;
        }
        dispatchLampAction(doc, millis());
      }
    }
  }

  if (pendingWifiOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len;
    portENTER_CRITICAL(&pendingMux);
    len = pendingWifiOpJson.length;
    memcpy(buf, pendingWifiOpJson.json, len);
    pendingWifiOpJson.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    buf[len] = '\0';

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain wifiOp len=%u\n", (unsigned)len);
#endif

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      const char* op = doc["op"].as<const char*>();
      if (op && strcmp(op, "scan") == 0) {
        wifi::startScan();
      } else if (op && strcmp(op, "connect") == 0) {
        std::string ssid(doc["ssid"] | "");
        std::string password(doc["password"] | "");
        if (!ssid.empty()) {
          config.homeMode.ssid = ssid;
          config.homeMode.password = password;
          wifi::connect(ssid, password);
        }
      } else if (op && strcmp(op, "forget") == 0) {
        config.homeMode.ssid.clear();
        config.homeMode.password.clear();
        wifi::forget();
      }
    }
  }

  if (pendingMqttOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len;
    portENTER_CRITICAL(&pendingMux);
    len = pendingMqttOpJson.length;
    memcpy(buf, pendingMqttOpJson.json, len);
    pendingMqttOpJson.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    buf[len] = '\0';

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain mqttOp len=%u\n", (unsigned)len);
#endif

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      const char* op = doc["op"].as<const char*>();
      if (op && strcmp(op, "update") == 0) {
        if (doc["enabled"].is<bool>())       config.mqtt.enabled    = doc["enabled"].as<bool>();
        if (doc["brokerHost"].is<const char*>()) config.mqtt.brokerHost = std::string(doc["brokerHost"].as<const char*>());
        if (doc["brokerPort"].is<int>())     config.mqtt.brokerPort = doc["brokerPort"].as<uint16_t>();
        if (doc["username"].is<const char*>())   config.mqtt.username = std::string(doc["username"].as<const char*>());
        // Password: "********" sentinel means "preserve stored value".
        if (doc["password"].is<const char*>()) {
          std::string p = doc["password"].as<const char*>();
          if (p != "********") {
            config.mqtt.password = p;
          }
        }
        if (doc["topicPrefix"].is<const char*>()) config.mqtt.topicPrefix = std::string(doc["topicPrefix"].as<const char*>());

        mqtt.applyConfig();
        ble_control::notifyMqttSection();
      }
    }
  }

  // Drain inbound ESP-NOW CONTROL_OP (deferred from ShowReceiver's WiFi
  // task) — JSON parse + local dispatch.
  if (pendingInboundOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len;
    portENTER_CRITICAL(&pendingMux);
    len = pendingInboundOpJson.length;
    memcpy(buf, pendingInboundOpJson.json, len);
    pendingInboundOpJson.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    buf[len] = '\0';
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain inboundOp len=%u\n", (unsigned)len);
#endif
    applyRemoteOpLocal(buf, len);
  }

  // Drain BLE CHAR_REMOTE_OP writes: either apply locally (targetMac is
  // self or "broadcast") or forward over ESP-NOW to the addressed peer.
  if (pendingRemoteOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len;
    portENTER_CRITICAL(&pendingMux);
    len = pendingRemoteOpJson.length;
    memcpy(buf, pendingRemoteOpJson.json, len);
    pendingRemoteOpJson.valid = false;
    portEXIT_CRITICAL(&pendingMux);
    buf[len] = '\0';
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain remoteOp len=%u\n", (unsigned)len);
#endif

    JsonDocument doc;
    if (deserializeJson(doc, buf, len) == DeserializationError::Ok) {
      const char* tgtStr = doc["targetMac"].as<const char*>();
      uint8_t targetMac[6] = {0};
      bool isBroadcast = false;
      bool isSelf = false;
      if (tgtStr) {
        if (strcmp(tgtStr, "broadcast") == 0) {
          memset(targetMac, 0xFF, 6);
          isBroadcast = true;
        } else if (sscanf(tgtStr, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                          &targetMac[0], &targetMac[1], &targetMac[2],
                          &targetMac[3], &targetMac[4], &targetMac[5]) == 6) {
          uint8_t myMac[6];
          showReceiver.getMyMac(myMac);
          isSelf = (memcmp(targetMac, myMac, 6) == 0);
        }
      }

      // Strip targetMac before forwarding/applying — the payload should be
      // the same shape applyRemoteOpLocal expects.
      doc.remove("targetMac");
      std::string payload;
      serializeJson(doc, payload);

      if (isSelf || isBroadcast) {
        applyRemoteOpLocal(payload.data(), payload.size());
      }
      if (!isSelf) {
        // Forward over the grid. For broadcast this also bounces to all
        // peers; for unicast it targets the specific MAC.
        showReceiver.sendControlOp(
            targetMac,
            reinterpret_cast<const uint8_t*>(payload.data()),
            payload.size());
      }
    }
  }

  wifi::tick();
  mqtt.tick();
  showReceiver.tick();

  compositor.tick();
};
