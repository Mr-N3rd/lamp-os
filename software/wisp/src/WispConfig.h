// WispConfig — thin NVS wrapper for wisp-side persistent settings.
//
// Phase D introduces the first persistent wisp state: a Flutter app pane will
// (a) pick which Aurora zone this wisp follows, and (b) eventually push WiFi
// credentials over BLE. Both ride in MSG_CONTROL_OP payloads (JSON), are
// dispatched by WispOpDispatcher, and land here for persistence.
//
// Storage: Arduino-ESP32 `Preferences` (NVS), namespace `"wisp"`. Keys are
// kept short because NVS imposes a 15-byte key length limit:
//   selZone   int32   selected Aurora zone, -1 = unset (0 is a valid zone)
//   wifiSsid  String  setWifi op storage — read in a later phase
//   wifiPw    String  setWifi op storage — read in a later phase
//
// The class caches the values in RAM after `begin()` so the read path doesn't
// hit NVS on every Aurora palette notification.

#pragma once

#include <Arduino.h>
#include <Preferences.h>

namespace wisp {

class WispConfig {
 public:
  // Open the `wisp` Preferences namespace in RW mode and cache the values.
  // Safe to call once at boot from setup().
  void begin();

  // -1 sentinel means "no zone selected yet". 0 is a valid Aurora zone, so we
  // can't use 0 as the unset sentinel.
  int selectedZone() const { return selectedZone_; }
  bool hasSelectedZone() const { return selectedZone_ >= 0; }

  // Write-through to NVS. Negative values are rejected (use clearSelectedZone).
  void setSelectedZone(int zone);

  // Removes the key from NVS and resets the cache to -1.
  void clearSelectedZone();

  // setWifi op stubs. Not consumed this phase — STA bring-up wiring lands in a
  // later task. Exposed now so the dispatcher doesn't have to grow its own
  // storage layer when setWifi arrives.
  const String& wifiSsid() const { return wifiSsid_; }
  const String& wifiPw() const { return wifiPw_; }
  bool hasWifi() const { return wifiSsid_.length() > 0; }
  void setWifi(const String& ssid, const String& pw);
  void clearWifi();

 private:
  Preferences prefs_;
  bool        opened_ = false;

  int    selectedZone_ = -1;
  String wifiSsid_;
  String wifiPw_;
};

}  // namespace wisp
