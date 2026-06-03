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
  // Print a compact, secret-free summary instead of the raw NVS JSON.
  // The raw payload would leak `lamp.password` to anyone on the serial
  // console (USB physical access). If you need the full shape while
  // debugging a parse bug, attach a temporary `Serial.println(json)`
  // for that session — don't roll back this redaction.
  {
    JsonDocument peek;
    if (deserializeJson(peek, json) == DeserializationError::Ok) {
      const char* loadedName = peek["lamp"]["name"] | "<unset>";
      const char* pwField = peek["lamp"]["password"] | "";
      const bool hasPassword = pwField != nullptr && pwField[0] != '\0';
      const int exprCount = peek["expressions"].is<JsonArray>()
                                ? (int)peek["expressions"].as<JsonArray>().size()
                                : 0;
      Serial.printf(
          "[cfg] loaded name=%s pw=%s expressions=%d nvs_bytes=%u\n",
          loadedName, hasPassword ? "set" : "unset", exprCount,
          (unsigned)json.length());
    } else {
      Serial.printf("[cfg] loaded nvs_bytes=%u (parse failed; full dump suppressed)\n",
                    (unsigned)json.length());
    }
  }
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
  // SocialMode persists as uint8_t (0=Introvert, 1=Ambivert, 2=Extrovert).
  // Out-of-range values fall back to Ambivert so a corrupt or future-versioned
  // payload doesn't strand the lamp in an unknown personality.
  {
    uint32_t modeRaw = lampNode["socialMode"] | 1;
    if (modeRaw > 2) modeRaw = 1;
    lamp.socialMode = static_cast<SocialMode>(modeRaw);
  }

  JsonObject baseNode = doc["base"];
  base.px = baseNode["px"] | 36;
  if (base.px > 50) {
    base.px = 50;
  }
  // Keep knockoutPixels in sync with the active pixel count. Drops stale
  // entries when px shrinks (e.g. 35 → 20) and 100-fills ("no knockout")
  // any slots when px grows. The input loop below then overwrites slots
  // 0..base.px-1 from the JSON.
  base.knockoutPixels.resize(base.px, 100);
  base.ac = baseNode["ac"] | 0;
  base.bpp = baseNode["bpp"] | 4;
  if (base.bpp != 3 && base.bpp != 4) {
    base.bpp = 4;  // defensive: only 3 or 4 valid
  }
  // byteOrder is the source of truth for strip type. When absent (legacy
  // payloads), default-derive from bpp so behavior is unchanged for
  // existing lamps. See docs/firmware-proposals/2026-05-29-neo-bgr-byte-order.md.
  const char* baseBoCstr = baseNode["byteOrder"] | "";
  base.byteOrder = baseBoCstr;
  if (base.byteOrder.empty()) {
    base.byteOrder = (base.bpp == 4) ? "GRBW" : "GRB";
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
  // Knockout: positional uint8 array, one entry per pixel. Index = pixel;
  // value = brightness % (0..100, default 100). Matches `asBaseJson` /
  // `asJsonDocument` emit shape.
  JsonArray baseKnockoutPixels = baseNode["knockout"];
  for (size_t i = 0;
       i < baseKnockoutPixels.size() && i < base.knockoutPixels.size();
       i++) {
    int value = baseKnockoutPixels[i] | 100;
    base.knockoutPixels[i] = (uint8_t)value;
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
  const char* shadeBoCstr = shadeNode["byteOrder"] | "";
  shade.byteOrder = shadeBoCstr;
  if (shade.byteOrder.empty()) {
    shade.byteOrder = (shade.bpp == 4) ? "GRBW" : "GRB";
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

  // Load home mode. Legacy NVS may still carry a "password" field — we
  // silently ignore it (presence-only home mode doesn't store passwords).
  JsonObject homeModeNode = doc["homeMode"];
  if (homeModeNode) {
    homeMode.ssid = std::string(homeModeNode["ssid"] | "");
    homeMode.brightness = homeModeNode["brightness"] | 60;
    // Migration: lamps configured before `enabled` existed have no
    // "enabled" key in their NVS-stored JSON. Treat "has SSID" as proxy
    // for "user wanted home mode on" so they keep working post-update.
    homeMode.enabled = homeModeNode["enabled"] | !homeMode.ssid.empty();
  }

  // (MQTT removed — legacy NVS "mqtt" block is silently ignored.)

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

  // Per-peer dispositions live in a separate NVS key — loaded last so the
  // main blob's prefs->begin/end pair above doesn't conflict.
  loadDispositionsFromPrefs_();
};

void Config::loadDispositionsFromPrefs_() {
  if (!prefs) return;
  prefs->begin("lamp", true);
  String json = prefs->getString("dispositions", "{}");
  prefs->end();

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;

  for (JsonPair kv : doc.as<JsonObject>()) {
    const std::string name(kv.key().c_str());
    uint32_t v = kv.value() | (uint32_t)kDispositionDefault;
    if (v < 1) v = 1;
    if (v > 5) v = 5;
    dispositions_[name] = static_cast<uint8_t>(v);
    if (dispositions_.size() >= kDispositionsMax) break;
  }
}

void Config::persistDispositions_() {
  if (!prefs) return;
  String out = asDispositionsJson();
  prefs->begin("lamp", false);
  prefs->putString("dispositions", out.c_str());
  prefs->end();
}

uint8_t Config::getDisposition(const std::string& peerName) const {
  auto it = dispositions_.find(peerName);
  if (it == dispositions_.end()) return kDispositionDefault;
  return it->second;
}

void Config::setDisposition(const std::string& peerName, uint8_t value) {
  if (peerName.empty()) return;
  if (value < 1) value = 1;
  if (value > 5) value = 5;
  if (dispositions_.size() >= kDispositionsMax &&
      dispositions_.find(peerName) == dispositions_.end()) {
    // Evict the first-by-name entry. Disposition tracking is best-effort
    // at the cap; users typically have <100 paired lamps.
    dispositions_.erase(dispositions_.begin());
  }
  dispositions_[peerName] = value;
  persistDispositions_();
}

String Config::asDispositionsJson() const {
  JsonDocument doc;
  for (const auto& kv : dispositions_) {
    doc[kv.first.c_str()] = kv.second;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

bool Config::setDispositionsFromJson(const char* json, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
    return false;
  }
  if (!doc.is<JsonObject>()) return false;
  dispositions_.clear();
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (dispositions_.size() >= kDispositionsMax) break;
    uint32_t v = kv.value() | (uint32_t)kDispositionDefault;
    if (v < 1) v = 1;
    if (v > 5) v = 5;
    dispositions_[std::string(kv.key().c_str())] = static_cast<uint8_t>(v);
  }
  persistDispositions_();
  return true;
}

JsonDocument Config::asJsonDocument() {
  JsonDocument doc;

  JsonObject lampNode = doc["lamp"].to<JsonObject>();
  lampNode["name"] = lamp.name;
  lampNode["brightness"] = lamp.brightness;
  if (!lamp.password.empty()) {
    lampNode["password"] = lamp.password;
  }
  lampNode["advancedEnabled"] = lamp.advancedEnabled;
  lampNode["socialMode"] = static_cast<uint8_t>(lamp.socialMode);
  JsonObject baseNode = doc["base"].to<JsonObject>();
  baseNode["px"] = base.px;
  baseNode["ac"] = base.ac;
  baseNode["bpp"] = base.bpp;
  baseNode["byteOrder"] = base.byteOrder;
  JsonArray baseColorsNode = baseNode["colors"].to<JsonArray>();
  for (int i = 0; i < base.colors.size(); i++) {
    baseColorsNode[i] = colorToHexString(base.colors[i]);
  }
  // Positional uint8 array, same shape as asBaseJson — keeps the on-disk
  // NVS format consistent with the BLE per-section read.
  JsonArray baseKnockoutNode = baseNode["knockout"].to<JsonArray>();
  for (int i = 0; i < base.knockoutPixels.size(); i++) {
    baseKnockoutNode.add((int)base.knockoutPixels[i]);
  }

  JsonObject shadeNode = doc["shade"].to<JsonObject>();
  shadeNode["px"] = shade.px;
  shadeNode["bpp"] = shade.bpp;
  shadeNode["byteOrder"] = shade.byteOrder;
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
  homeModeNode["brightness"] = homeMode.brightness;
  homeModeNode["enabled"] = homeMode.enabled;

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
  doc["socialMode"] = static_cast<uint8_t>(lamp.socialMode);
  String out;
  serializeJson(doc, out);
  return out;
}

String Config::asBaseJson() {
  JsonDocument doc;
  doc["px"] = base.px;
  doc["ac"] = base.ac;
  doc["bpp"] = base.bpp;
  doc["byteOrder"] = base.byteOrder;
  JsonArray colorsNode = doc["colors"].to<JsonArray>();
  for (size_t i = 0; i < base.colors.size(); i++) {
    colorsNode.add(colorToHexString(base.colors[i]));
  }
  // Knockout: positional uint8 array, one entry per pixel.
  //   wire shape: "knockout":[100,100,6,9,...] (length = knockoutPixels.size())
  // Index = pixel; value = brightness % (0..100). Caps the base section at
  // ~200 bytes regardless of pattern density so a dense knockout fits
  // under the per-characteristic ATT cap.
  JsonArray knockoutNode = doc["knockout"].to<JsonArray>();
  for (size_t i = 0; i < base.knockoutPixels.size(); i++) {
    knockoutNode.add((int)base.knockoutPixels[i]);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String Config::asShadeJson() {
  JsonDocument doc;
  doc["px"] = shade.px;
  doc["bpp"] = shade.bpp;
  doc["byteOrder"] = shade.byteOrder;
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
  doc["brightness"] = homeMode.brightness;
  doc["enabled"] = homeMode.enabled;
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace lamp