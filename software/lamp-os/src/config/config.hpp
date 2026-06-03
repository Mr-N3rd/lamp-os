#ifndef LAMP_CONFIG_CONFIG_H
#define LAMP_CONFIG_CONFIG_H
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <map>
#include <string>

#include "./config_types.hpp"

namespace lamp {
/**
 * @brief configurations file for the lamp that can be modified on the web
 * portal
 * @property lamp - global lamp details
 * @property base - details about the neopixels in the lamp base
 * @property shade - details about the neopixels in the lamp bulb
 */
class Config {
 private:
  Preferences* prefs;

 public:
  LampSettings lamp;
  BaseSettings base;
  ShadeSettings shade;
  ExpressionSettings expressions;
  HomeModeSettings homeMode;

  Config() {};

  /**
   * @brief create a config based on information in the user's storage
   * @param [in] inPrefs preferences container for nvs values
   */
  Config(Preferences* inPrefs);

  /**
   * @brief create a streamable json doc to send configs to the webserver
   * @return a JsonDocument to serialize
   */
  JsonDocument asJsonDocument();

  // Per-section serializers — each returns a String of just the JSON for
  // that section. Used by the split CHAR_*_SECTION characteristics so each
  // stays well under MTU.
  String asLampJson();
  String asBaseJson();
  String asShadeJson();
  String asExpressionsJson();
  String asHomeModeJson();

  // Per-peer social disposition (1=salty .. 3=neutral .. 5=smitten). Lives
  // in a SEPARATE NVS key ("dispositions") from the main config blob so the
  // peer list can grow without bloating CHAR_LAMP_SECTION / settings_blob.
  // Stored as JSON object { "peerName": 1..5 }. Bounded to ~100 entries.
  // Per-lamp metadata — never synced cross-mesh; each lamp has its own view.
  static constexpr uint8_t kDispositionDefault = 3;
  static constexpr size_t kDispositionsMax = 100;

  // Returns kDispositionDefault when the peer isn't in the map.
  uint8_t getDisposition(const std::string& peerName) const;
  // Clamps `value` to [1,5]. Persists immediately. Evicts oldest-updated
  // entry if at kDispositionsMax (keyed by name; we don't track update
  // timestamps — eviction is "first by std::map iteration order" which is
  // alphabetical-by-name; fine for an at-capacity scenario that shouldn't
  // be reached in practice).
  void setDisposition(const std::string& peerName, uint8_t value);
  // Full JSON serialization for the CHAR_SOCIAL_DISPOSITIONS read path.
  String asDispositionsJson() const;
  // Bulk replace from the CHAR_SOCIAL_DISPOSITIONS write path. Caller
  // provides a JSON object; we parse, clamp, persist. Returns true on
  // success.
  bool setDispositionsFromJson(const char* json, size_t len);

 private:
  std::map<std::string, uint8_t> dispositions_;
  void loadDispositionsFromPrefs_();
  void persistDispositions_();
};
}  // namespace lamp

#endif