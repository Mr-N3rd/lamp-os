#ifndef LAMP_CONFIG_CONFIG_H
#define LAMP_CONFIG_CONFIG_H
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <map>
#include <string>

#include "./config_types.hpp"

namespace lamp {

// Pure clock-only "should I flush?" helper used by Config to debounce
// NVS writes during disposition slider drags. NO NVS access, no I/O —
// just a dirty flag and the timestamp of the most recent mutation.
//
// Audit context: setDisposition + setDispositionsFromJson used to call
// persistDispositions_ on every update, so a single slider drag (~20
// values per peer) could chew through NVS page wear in years not
// decades. We now mark dirty + record now() and let the loop drain on
// Core 1 flush once the user stops fiddling for kDispositionFlushIdleMs.
//
// Tested in test/test_disposition_debounce — the shape is mirrored
// inline there to keep the native env free of Arduino/NVS dependencies.
// If you change the API here, mirror the test class.
class DispositionDebouncer {
 public:
  explicit DispositionDebouncer(uint32_t idleMs) : idleMs_(idleMs) {}

  void markDirty(uint32_t nowMs) {
    dirty_ = true;
    lastMarkMs_ = nowMs;
  }

  bool dirty() const { return dirty_; }

  // Returns true iff dirty AND the idle window has elapsed since the
  // most recent markDirty. Caller is responsible for the actual flush
  // and then calling clear(). Subtraction-based comparison so millis()
  // wraparound (every ~49 days) doesn't strand the dirty flag forever.
  bool shouldFlush(uint32_t nowMs) const {
    if (!dirty_) return false;
    return (nowMs - lastMarkMs_) >= idleMs_;
  }

  void clear() {
    dirty_ = false;
    lastMarkMs_ = 0;
  }

 private:
  bool dirty_ = false;
  uint32_t lastMarkMs_ = 0;
  uint32_t idleMs_;
};

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
  // Idle window before debounced disposition writes are committed to NVS.
  // See DispositionDebouncer and audit finding #5 (NVS write amplification).
  // 5s comfortably exceeds a worst-case slider-drag cadence (~20 Hz BLE
  // writes) while still feeling snappy if the user closes the app right
  // after touching the slider (the BLE disconnect path forces a flush).
  static constexpr uint32_t kDispositionFlushIdleMs = 5000;

  // Returns kDispositionDefault when the peer isn't in the map.
  uint8_t getDisposition(const std::string& peerName) const;
  // Clamps `value` to [1,5]. Marks the debouncer dirty — the actual NVS
  // write happens later via maybeFlushDispositions() or
  // flushDispositionsNow(). Evicts oldest-updated entry if at
  // kDispositionsMax (keyed by name; we don't track update timestamps —
  // eviction is "first by std::map iteration order" which is
  // alphabetical-by-name; fine for an at-capacity scenario that shouldn't
  // be reached in practice).
  void setDisposition(const std::string& peerName, uint8_t value);
  // Full JSON serialization for the CHAR_SOCIAL_DISPOSITIONS read path.
  String asDispositionsJson() const;
  // Bulk replace from the CHAR_SOCIAL_DISPOSITIONS write path. Caller
  // provides a JSON object; we parse, clamp, mark dirty. The actual NVS
  // commit is deferred to the next maybeFlushDispositions/flushDispositionsNow
  // call. Returns true on success.
  bool setDispositionsFromJson(const char* json, size_t len);

  // Called from the loop drain on Core 1 every iteration. Cheap when no
  // disposition writes have happened — just a dirty-flag check + a
  // subtraction. Triggers persistDispositions_() once the user has been
  // idle for kDispositionFlushIdleMs and clears the dirty flag.
  void maybeFlushDispositions(uint32_t nowMs);
  // Synchronous flush for situations where deferring is unsafe (BLE
  // disconnect — phone is gone, we may lose power before the next loop
  // tick). Must be called on Core 1 (NVS is not Core-0-safe). No-op when
  // not dirty so onDisconnect can call it unconditionally.
  void flushDispositionsNow();

 private:
  std::map<std::string, uint8_t> dispositions_;
  DispositionDebouncer dispositionsDebouncer_{kDispositionFlushIdleMs};
  void loadDispositionsFromPrefs_();
  void persistDispositions_();
};
}  // namespace lamp

#endif