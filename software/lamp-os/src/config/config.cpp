#include "config.hpp"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <algorithm>

#include "util/color.hpp"
#include "version.hpp"

namespace lamp {

namespace {
// Comparator for std::lower_bound over the sorted dispositions vector.
// Compares an existing entry's name against the target lookup key.
// Used by getDisposition and setDisposition to find the insertion point
// in O(log N) while keeping the vector contiguous and cache-friendly.
inline bool dispositionEntryLess(
    const std::pair<std::string, uint8_t>& a, const std::string& b) {
  return a.first < b;
}
}  // namespace
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
  lamp.name = std::string(lampNode["name"] | "stray");
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
      // (Refactor 2026-06-13: disabledDuringWispOverride parse removed —
      // now a pure type-property on the Expression subclass; nothing to
      // load from NVS. Old NVS blobs with the key are tolerated; the
      // skip-list below drops it from the generic parameter loop so it
      // isn't accidentally captured as a parameter value.)
      // Load generic parameters
      for (JsonPair kv : exprNode) {
        const char* key = kv.key().c_str();
        std::string keyStr(key);

        // Skip common fields we've already handled
        if (keyStr == "type" || keyStr == "enabled" || keyStr == "intervalMin" ||
            keyStr == "intervalMax" || keyStr == "target" || keyStr == "colors" ||
            keyStr == "disabledDuringWispOverride") {
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

  // Two-pass load: gather into a fresh vector, then sort once. This is
  // O(N log N) vs O(N^2) if we used setDisposition() in a loop (each
  // call would memmove the tail on insert). Reserve to skip reallocation
  // for typical loads.
  dispositions_.clear();
  dispositions_.reserve(kDispositionsMax);
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (dispositions_.size() >= kDispositionsMax) break;
    const std::string name(kv.key().c_str());
    if (name.empty()) continue;
    uint32_t v = kv.value() | (uint32_t)kDispositionDefault;
    if (v < 1) v = 1;
    if (v > 5) v = 5;
    dispositions_.emplace_back(name, static_cast<uint8_t>(v));
  }
  std::sort(dispositions_.begin(), dispositions_.end(),
            [](const std::pair<std::string, uint8_t>& a,
               const std::pair<std::string, uint8_t>& b) {
              return a.first < b.first;
            });
  // Dedupe defensively — JSON objects don't have duplicate keys per spec,
  // but ArduinoJson is permissive on bad input. Last write wins, matching
  // the prior std::map behaviour.
  auto last = std::unique(
      dispositions_.begin(), dispositions_.end(),
      [](const std::pair<std::string, uint8_t>& a,
         const std::pair<std::string, uint8_t>& b) { return a.first == b.first; });
  dispositions_.erase(last, dispositions_.end());
}

bool Config::persistConfig(const char* via) {
  if (!prefs) return false;
  JsonDocument doc = asJsonDocument();
  String out;
  serializeJson(doc, out);
  // Match persistDispositions_'s defensive pattern — prefs.begin can fail
  // when NVS is full or the partition is corrupt; a putString against an
  // unopened handle silently writes nothing. Skip the write and let the
  // caller decide (callers today just move on; expression edits stay in
  // RAM and the next persistConfig attempt may succeed).
  if (!prefs->begin("lamp", false)) {
#ifdef LAMP_DEBUG
    Serial.println("[nvs] prefs.begin failed (persistConfig)");
#endif
    return false;
  }
  size_t written = prefs->putString("cfg", out.c_str());
  prefs->end();
#ifdef LAMP_DEBUG
  if (written == 0) {
    Serial.println("[nvs] persistConfig putString wrote 0 bytes");
  } else {
    Serial.printf("[nvs] persistConfig via=%s wrote %u bytes\n",
                  via, (unsigned)written);
  }
#endif
  return written > 0;
}

bool Config::persistDispositions_() {
  if (!prefs) return false;
  String out = asDispositionsJson();
  // Audit fix: prefs.begin returns false when NVS is full or the partition
  // is corrupt. A subsequent putString against an unopened handle silently
  // writes to nothing. Skip the write and let the debouncer keep its dirty
  // flag so the next flush attempt retries.
  if (!prefs->begin("lamp", false)) {
#ifdef LAMP_DEBUG
    Serial.println("[nvs] prefs.begin failed (dispositions persist)");
#endif
    return false;
  }
  size_t written = prefs->putString("dispositions", out.c_str());
  prefs->end();
#ifdef LAMP_DEBUG
  if (written == 0) {
    Serial.println("[nvs] dispositions putString wrote 0 bytes");
  }
#endif
  return written > 0;
}

uint8_t Config::getDisposition(const std::string& peerName) const {
  // Binary search on the sorted vector. lower_bound returns the first
  // entry >= peerName; we must still compare names because lower_bound
  // can land on a strictly-greater neighbour (e.g. looking up "bob" in
  // {alice, charlie} returns the iterator to charlie).
  auto it = std::lower_bound(dispositions_.begin(), dispositions_.end(),
                             peerName, dispositionEntryLess);
  if (it == dispositions_.end() || it->first != peerName) {
    return kDispositionDefault;
  }
  return it->second;
}

void Config::setDisposition(const std::string& peerName, uint8_t value) {
  if (peerName.empty()) return;
  if (value < 1) value = 1;
  if (value > 5) value = 5;
  auto it = std::lower_bound(dispositions_.begin(), dispositions_.end(),
                             peerName, dispositionEntryLess);
  if (it != dispositions_.end() && it->first == peerName) {
    // Update in place — no resize, no shift, no eviction. Preserves sort
    // order trivially.
    it->second = value;
  } else {
    if (dispositions_.size() >= kDispositionsMax) {
      // Evict the lowest-by-name entry to match the historical std::map
      // iteration-order eviction policy. Disposition tracking is
      // best-effort at the cap; users typically have <100 paired lamps.
      dispositions_.erase(dispositions_.begin());
      // The insertion point may have shifted by one after erase; recompute.
      it = std::lower_bound(dispositions_.begin(), dispositions_.end(),
                            peerName, dispositionEntryLess);
    }
    dispositions_.insert(it, std::make_pair(peerName, value));
  }
  // Audit fix: do NOT persist here. The slider-drag UX dragged ~20
  // writes per peer per drag; multiplied across multiple peers and
  // years of ownership, the 100k-write-per-page NVS budget is reachable.
  // The loop drain on Core 1 polls maybeFlushDispositions and writes
  // once the user stops touching the slider for kDispositionFlushIdleMs.
  // BLE disconnect path force-flushes via flushDispositionsNow. Factory
  // reset doesn't need a flush — it erases NVS wholesale.
  dispositionsDebouncer_.markDirty(millis());
}

String Config::asDispositionsJson() const {
  // Sorted-vector iteration yields keys in lexicographic order — stable
  // round-trips across reads (the prior std::map also iterated in sorted
  // order, so on-disk byte shape is unchanged).
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
  // Bulk replace: stage into a fresh local vector, then sort once. Same
  // O(N log N) strategy as loadDispositionsFromPrefs_ — avoids repeated
  // O(N) shifts that an N-call sequence of setDisposition() would incur.
  std::vector<std::pair<std::string, uint8_t>> next;
  next.reserve(kDispositionsMax);
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (next.size() >= kDispositionsMax) break;
    std::string name(kv.key().c_str());
    if (name.empty()) continue;
    uint32_t v = kv.value() | (uint32_t)kDispositionDefault;
    if (v < 1) v = 1;
    if (v > 5) v = 5;
    next.emplace_back(std::move(name), static_cast<uint8_t>(v));
  }
  std::sort(next.begin(), next.end(),
            [](const std::pair<std::string, uint8_t>& a,
               const std::pair<std::string, uint8_t>& b) {
              return a.first < b.first;
            });
  auto last = std::unique(
      next.begin(), next.end(),
      [](const std::pair<std::string, uint8_t>& a,
         const std::pair<std::string, uint8_t>& b) { return a.first == b.first; });
  next.erase(last, next.end());
  dispositions_ = std::move(next);
  // Audit fix: defer persistence. CHAR_SOCIAL_DISPOSITIONS bulk writes
  // arrive on every slider drag from the app, each re-serialising the
  // full blob — the worst case for NVS wear. The Core 1 loop drain
  // (maybeFlushDispositions) commits once idle; the BLE onDisconnect
  // post forces a synchronous commit when the phone walks away.
  dispositionsDebouncer_.markDirty(millis());
  return true;
}

void Config::maybeFlushDispositions(uint32_t nowMs) {
  if (!dispositionsDebouncer_.shouldFlush(nowMs)) return;
  // Only clear the debouncer on a successful write; on failure leave it
  // dirty so the next flush attempt retries (safer default — the user's
  // slider input has nowhere else to go).
  if (persistDispositions_()) {
    dispositionsDebouncer_.clear();
  }
}

void Config::flushDispositionsNow() {
  if (!dispositionsDebouncer_.dirty()) return;
  if (persistDispositions_()) {
    dispositionsDebouncer_.clear();
  }
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
    // disabledDuringWispOverride is no longer persisted — pure type-property.
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
  // Firmware identity (packed semver + release channel string). Constant
  // at boot, so no extra invalidation hook is needed — the existing lamp
  // section cache picks these up the first time it's built.
  doc["fwVersion"] = FIRMWARE_VERSION;
  doc["fwChannel"] = FIRMWARE_CHANNEL_STR;
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
    // disabledDuringWispOverride is no longer serialised — pure type-property.
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

// ── Per-section JSON cache (audit fix #6) ────────────────────────────────
//
// Each accessor:
//   - If the section is clean, returns the existing cached std::string ref
//     in O(1) — no JsonDocument allocation, no vector walk.
//   - If dirty, calls the existing asXJson() builder (which itself builds
//     a JsonDocument + walks colors/knockoutPixels/etc.), copies into the
//     member std::string, clears the dirty flag.
//
// Thread safety: see config.hpp — only call from Core 1. The BLE onRead
// callbacks on Core 0 hand back the already-pushed string from NimBLE's
// internal buffer (ble_control::tick on Core 1 pushes after rebuild).

const std::string& Config::lampSectionJsonCached() {
  if (lampSectionDirty_) {
    String s = asLampJson();
    lampSectionJson_.assign(s.c_str(), s.length());
    lampSectionDirty_ = false;
  }
  return lampSectionJson_;
}

const std::string& Config::baseSectionJsonCached() {
  if (baseSectionDirty_) {
    String s = asBaseJson();
    baseSectionJson_.assign(s.c_str(), s.length());
    baseSectionDirty_ = false;
  }
  return baseSectionJson_;
}

const std::string& Config::shadeSectionJsonCached() {
  if (shadeSectionDirty_) {
    String s = asShadeJson();
    shadeSectionJson_.assign(s.c_str(), s.length());
    shadeSectionDirty_ = false;
  }
  return shadeSectionJson_;
}

const std::string& Config::expressionsSectionJsonCached() {
  if (expressionsSectionDirty_) {
    String s = asExpressionsJson();
    expressionsSectionJson_.assign(s.c_str(), s.length());
    expressionsSectionDirty_ = false;
  }
  return expressionsSectionJson_;
}

const std::string& Config::homeSectionJsonCached() {
  if (homeSectionDirty_) {
    String s = asHomeModeJson();
    homeSectionJson_.assign(s.c_str(), s.length());
    homeSectionDirty_ = false;
  }
  return homeSectionJson_;
}

const std::string& Config::settingsBlobJsonCached() {
  if (settingsBlobDirty_) {
    JsonDocument doc = asJsonDocument();
    String s;
    serializeJson(doc, s);
    settingsBlobJson_.assign(s.c_str(), s.length());
    settingsBlobDirty_ = false;
  }
  return settingsBlobJson_;
}

void Config::invalidateLampSection() {
  lampSectionDirty_ = true;
  settingsBlobDirty_ = true;
}

void Config::invalidateBaseSection() {
  baseSectionDirty_ = true;
  settingsBlobDirty_ = true;
}

void Config::invalidateShadeSection() {
  shadeSectionDirty_ = true;
  settingsBlobDirty_ = true;
}

void Config::invalidateExpressionsSection() {
  expressionsSectionDirty_ = true;
  settingsBlobDirty_ = true;
}

void Config::invalidateHomeSection() {
  homeSectionDirty_ = true;
  settingsBlobDirty_ = true;
}

void Config::invalidateAllSections() {
  lampSectionDirty_ = true;
  baseSectionDirty_ = true;
  shadeSectionDirty_ = true;
  expressionsSectionDirty_ = true;
  homeSectionDirty_ = true;
  settingsBlobDirty_ = true;
}

}  // namespace lamp