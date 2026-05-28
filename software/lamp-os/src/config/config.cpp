#include "./config.hpp"

#include <ArduinoJson.h>
#include <Preferences.h>

#include "../util/color.hpp"

namespace lamp {
Config::Config(Preferences* inPrefs) {
  JsonDocument doc;
  prefs = inPrefs;
  prefs->begin("lamp", true);
  String json = prefs->getString("cfg", "{}");
  DeserializationError error = deserializeJson(doc, json);
  prefs->end();

#ifdef LAMP_DEBUG
  Serial.println(json);
#endif

  if (error) {
#ifdef LAMP_DEBUG
    Serial.printf("ws deserializeJson() failed: %s\n", error.c_str());
#endif
    return;  // use class defaults
  }

  JsonObject lampNode = doc["lamp"];
  lamp.name = std::string(lampNode["name"] | "standard");
  lamp.brightness = lampNode["brightness"] | 100;
  std::string password = std::string(lampNode["password"] | "");
  if (!password.empty()) {
    lamp.password = password;
  }
  lamp.advancedEnabled = lampNode["advancedEnabled"] | false;

  JsonObject baseNode = doc["base"];
  base.px = baseNode["px"] | 36;
  if (base.px > 50) {
    base.px = 50;
  }
  base.ac = baseNode["ac"] | 0;
  base.bpp = baseNode["bpp"] | 4;
  if (base.bpp != 3 && base.bpp != 4) {
    base.bpp = 4;  // defensive: only 3 or 4 valid
  }

  JsonArray baseColors = baseNode["colors"];
  int colorCollectionSize = baseColors.size();
  if (base.ac > colorCollectionSize - 1) {
    base.ac = 0;
  }

  if (colorCollectionSize > 0) {
    base.colors.clear();
    for (JsonVariant baseColor : baseColors) {
      base.colors.push_back(hexStringToColor(baseColor));
    }
  }
  JsonArray baseKnockoutPixels = baseNode["knockout"];
  if (baseKnockoutPixels.size()) {
    for (JsonObject baseKnockoutPixel : baseKnockoutPixels) {
      int pixelIndex = baseKnockoutPixel["p"] | 0;
      if (pixelIndex > 49) {
        continue;
      }

      base.knockoutPixels[pixelIndex] = baseKnockoutPixel["b"] | 100;
    }
  }

  JsonObject shadeNode = doc["shade"];
  shade.px = shadeNode["px"] | 38;
  if (shade.px > 50) {
    shade.px = 50;
  }
  shade.bpp = shadeNode["bpp"] | 4;
  if (shade.bpp != 3 && shade.bpp != 4) {
    shade.bpp = 4;
  }
  JsonArray shadeColors = shadeNode["colors"];
  if (shadeColors.size()) {
    shade.colors.clear();
    for (JsonVariant shadeColor : shadeColors) {
      shade.colors.push_back(hexStringToColor(shadeColor));
    }
  }

  // Load expressions
  JsonArray expressionsNode = doc["expressions"];
  if (expressionsNode) {
    expressions.expressions.clear();
    for (JsonObject exprNode : expressionsNode) {
      ExpressionConfig expr;
      expr.type = std::string(exprNode["type"] | "");
      expr.enabled = exprNode["enabled"] | false;
      expr.intervalMin = exprNode["intervalMin"] | 60;
      expr.intervalMax = exprNode["intervalMax"] | 900;
      expr.target = exprNode["target"] | 3;
      // Load generic parameters
      for (JsonPair kv : exprNode) {
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
      }

      JsonArray exprColors = exprNode["colors"];
      if (exprColors.size()) {
        for (JsonVariant color : exprColors) {
          expr.colors.push_back(hexStringToColor(color));
        }
      }

      expressions.expressions.push_back(expr);
    }
  }

  // Load home mode
  JsonObject homeModeNode = doc["homeMode"];
  if (homeModeNode) {
    homeMode.ssid = std::string(homeModeNode["ssid"] | "");
    homeMode.password = std::string(homeModeNode["password"] | "");
    homeMode.brightness = homeModeNode["brightness"] | 60;
  }

  // Load MQTT (smart-home / Home Assistant) config
  JsonObject mqttNode = doc["mqtt"];
  if (mqttNode) {
    mqtt.enabled = mqttNode["enabled"] | false;
    mqtt.brokerHost = std::string(mqttNode["brokerHost"] | "");
    mqtt.brokerPort = mqttNode["brokerPort"] | 1883;
    mqtt.username = std::string(mqttNode["username"] | "");
    mqtt.password = std::string(mqttNode["password"] | "");
    mqtt.topicPrefix = std::string(mqttNode["topicPrefix"] | "");
  }

  // Ensure both color vectors have at least one entry. Empty NVS (or NVS
  // erased / corrupted) returns "{}" with no colors arrays; downstream code
  // (e.g. bt.begin in standard_lamp.cpp uses base.colors[ac] and shade.colors[0])
  // calls operator[] on a std::vector, which is UB on empty and crashes boot
  // with an invalid-PC fault. Default to a visible white so the lamp at least
  // boots and the user can adjust colors via the app.
  if (base.colors.empty()) {
    base.colors.push_back(Color{255, 255, 255, 0});
  }
  if (shade.colors.empty()) {
    shade.colors.push_back(Color{255, 255, 255, 0});
  }
  if (base.ac >= base.colors.size()) {
    base.ac = 0;
  }
};

JsonDocument Config::asJsonDocument() {
  JsonDocument doc;

  JsonObject lampNode = doc["lamp"].to<JsonObject>();
  lampNode["name"] = lamp.name;
  lampNode["brightness"] = lamp.brightness;
  if (!lamp.password.empty()) {
    lampNode["password"] = lamp.password;
  }
  lampNode["advancedEnabled"] = lamp.advancedEnabled;
  JsonObject baseNode = doc["base"].to<JsonObject>();
  baseNode["px"] = base.px;
  baseNode["ac"] = base.ac;
  baseNode["bpp"] = base.bpp;
  JsonArray baseColorsNode = baseNode["colors"].to<JsonArray>();
  for (int i = 0; i < base.colors.size(); i++) {
    baseColorsNode[i] = colorToHexString(base.colors[i]);
  }
  JsonArray baseKnockoutNode = baseNode["knockout"].to<JsonArray>();
  int currentIdx = 0;
  for (int i = 0; i < base.knockoutPixels.size(); i++) {
    int value = base.knockoutPixels[i];

    // only send values that aren't 100% brightness as an optimization
    if (value == 100) {
      continue;
    }

    JsonObject baseKnockoutObjectNode = baseKnockoutNode[currentIdx].to<JsonObject>();
    baseKnockoutObjectNode["p"] = i;
    baseKnockoutObjectNode["b"] = value;

    currentIdx++;
  }

  JsonObject shadeNode = doc["shade"].to<JsonObject>();
  shadeNode["px"] = shade.px;
  shadeNode["bpp"] = shade.bpp;
  JsonArray shadeColorsNode = shadeNode["colors"].to<JsonArray>();
  for (int i = 0; i < shade.colors.size(); i++) {
    shadeColorsNode[i] = colorToHexString(shade.colors[i]);
  }

  // Serialize expressions
  JsonArray expressionsNode = doc["expressions"].to<JsonArray>();
  for (const auto& expr : expressions.expressions) {
    JsonObject exprNode = expressionsNode.add<JsonObject>();
    exprNode["type"] = expr.type;
    exprNode["enabled"] = expr.enabled;
    exprNode["intervalMin"] = expr.intervalMin;
    exprNode["intervalMax"] = expr.intervalMax;
    exprNode["target"] = expr.target;
    // Serialize generic parameters
    for (const auto& param : expr.parameters) {
      const std::string& key = param.first;
      const uint32_t& value = param.second;
      exprNode[key] = value;
    }

    JsonArray colorsNode = exprNode["colors"].to<JsonArray>();
    for (const auto& color : expr.colors) {
      colorsNode.add(colorToHexString(color));
    }
  }

  JsonObject homeModeNode = doc["homeMode"].to<JsonObject>();
  homeModeNode["ssid"] = homeMode.ssid;
  if (!homeMode.password.empty()) {
    homeModeNode["password"] = homeMode.password;
  }
  homeModeNode["brightness"] = homeMode.brightness;

  JsonObject mqttNode = doc["mqtt"].to<JsonObject>();
  mqttNode["enabled"] = mqtt.enabled;
  mqttNode["brokerHost"] = mqtt.brokerHost;
  mqttNode["brokerPort"] = mqtt.brokerPort;
  mqttNode["username"] = mqtt.username;
  mqttNode["password"] = mqtt.password;
  mqttNode["topicPrefix"] = mqtt.topicPrefix;

  return doc;
};

String Config::asLampJson() {
  JsonDocument doc;
  doc["name"] = lamp.name;
  doc["brightness"] = lamp.brightness;
  if (!lamp.password.empty()) {
    doc["password"] = lamp.password;
  }
  doc["advancedEnabled"] = lamp.advancedEnabled;
  String out;
  serializeJson(doc, out);
  return out;
}

String Config::asBaseJson() {
  JsonDocument doc;
  doc["px"] = base.px;
  doc["ac"] = base.ac;
  doc["bpp"] = base.bpp;
  JsonArray colorsNode = doc["colors"].to<JsonArray>();
  for (size_t i = 0; i < base.colors.size(); i++) {
    colorsNode.add(colorToHexString(base.colors[i]));
  }
  JsonArray knockoutNode = doc["knockout"].to<JsonArray>();
  for (size_t i = 0; i < base.knockoutPixels.size(); i++) {
    int value = base.knockoutPixels[i];
    if (value == 100) continue;  // omit defaults
    JsonObject entry = knockoutNode.add<JsonObject>();
    entry["p"] = i;
    entry["b"] = value;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String Config::asShadeJson() {
  JsonDocument doc;
  doc["px"] = shade.px;
  doc["bpp"] = shade.bpp;
  JsonArray colorsNode = doc["colors"].to<JsonArray>();
  for (size_t i = 0; i < shade.colors.size(); i++) {
    colorsNode.add(colorToHexString(shade.colors[i]));
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String Config::asExpressionsJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& expr : expressions.expressions) {
    JsonObject exprNode = arr.add<JsonObject>();
    exprNode["type"] = expr.type;
    exprNode["enabled"] = expr.enabled;
    exprNode["intervalMin"] = expr.intervalMin;
    exprNode["intervalMax"] = expr.intervalMax;
    exprNode["target"] = expr.target;
    for (const auto& param : expr.parameters) {
      exprNode[param.first] = param.second;
    }
    JsonArray colorsNode = exprNode["colors"].to<JsonArray>();
    for (const auto& color : expr.colors) {
      colorsNode.add(colorToHexString(color));
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String Config::asHomeModeJson() {
  JsonDocument doc;
  doc["ssid"] = homeMode.ssid;
  // Mask the password — the app only needs to know "is one set". User who
  // wants to change it must type a new one. BLE link is encrypted post-pair,
  // but defense-in-depth: app memory should not hold a round-trippable copy.
  if (!homeMode.password.empty()) {
    doc["password"] = "********";
  }
  doc["brightness"] = homeMode.brightness;
  String out;
  serializeJson(doc, out);
  return out;
}

String Config::asMqttJson() {
  JsonDocument doc;
  doc["enabled"] = mqtt.enabled;
  doc["brokerHost"] = mqtt.brokerHost;
  doc["brokerPort"] = mqtt.brokerPort;
  doc["username"] = mqtt.username;
  // Mask broker password — same rationale as home mode above. App must
  // send the literal sentinel back on update to indicate "leave unchanged".
  if (!mqtt.password.empty()) {
    doc["password"] = "********";
  }
  doc["topicPrefix"] = mqtt.topicPrefix;
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace lamp