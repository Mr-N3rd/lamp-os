#include "WispConfig.h"

#include <algorithm>

namespace wisp {

namespace {
constexpr const char* kNamespace      = "wisp";
constexpr const char* kKeyZone        = "selZone";
constexpr const char* kKeySsid        = "wifiSsid";
constexpr const char* kKeyPw          = "wifiPw";
// Phase E source-mode persistence. Single u8: 0=Off, 1=Manual, 2=Aurora.
// Stored as int to match the Preferences API; the runtime cast clamps
// out-of-range values to Aurora.
constexpr const char* kKeySourceMode  = "srcMode";
// Phase E manual palette persistence. Packed RGB bytes — 3 bytes per
// color, up to kManualPaletteMaxColors. Stored as a blob so the count is
// implicit in the byte length (length % 3 == 0). Empty blob → empty
// palette.
constexpr const char* kKeyManualPalette = "manualPal";

WispSourceMode coerceSourceMode(int raw) {
  switch (raw) {
    case static_cast<int>(WispSourceMode::Off):
    case static_cast<int>(WispSourceMode::Manual):
    case static_cast<int>(WispSourceMode::Aurora):
      return static_cast<WispSourceMode>(raw);
    default:
      // Any unknown / corrupted persisted value falls back to Aurora so
      // a partially-flashed wisp doesn't drop into a stranded "Off" mode
      // with no operator nearby to fix it.
      return WispSourceMode::Aurora;
  }
}
}  // namespace

void WispConfig::begin() {
  if (opened_) return;
  // Preferences::begin(name, readonly=false). The Arduino-ESP32 API auto-
  // creates the namespace on first write, but opening RW ensures subsequent
  // sets won't have to reopen.
  opened_ = prefs_.begin(kNamespace, /*readOnly=*/false);
  if (!opened_) {
    Serial.println("[wisp.cfg] Preferences::begin('wisp') failed");
    selectedZone_ = -1;
    wifiSsid_     = String();
    wifiPw_       = String();
    return;
  }
  // getInt second arg is the default returned when key is missing.
  selectedZone_ = prefs_.getInt(kKeyZone, -1);
  wifiSsid_     = prefs_.getString(kKeySsid, String());
  wifiPw_       = prefs_.getString(kKeyPw, String());
  // Default to Aurora when the key is missing — matches the pre-Phase-E
  // behavior where every wisp followed Aurora.
  sourceMode_   = coerceSourceMode(
      prefs_.getInt(kKeySourceMode,
                    static_cast<int>(WispSourceMode::Aurora)));

  // Manual palette: bytes blob, 3 bytes per color, capped at the
  // protocol max. getBytesLength returns 0 for missing keys.
  manualPalette_.clear();
  const size_t paletteLen = prefs_.getBytesLength(kKeyManualPalette);
  if (paletteLen > 0 && paletteLen % 3 == 0) {
    const size_t colorCount =
        std::min<size_t>(paletteLen / 3, kManualPaletteMaxColors);
    uint8_t buf[kManualPaletteMaxColors * 3];
    const size_t toRead = colorCount * 3;
    const size_t got = prefs_.getBytes(kKeyManualPalette, buf, toRead);
    if (got == toRead) {
      manualPalette_.reserve(colorCount);
      for (size_t i = 0; i < colorCount; ++i) {
        ManualPaletteColor c;
        c.r = buf[i * 3 + 0];
        c.g = buf[i * 3 + 1];
        c.b = buf[i * 3 + 2];
        manualPalette_.push_back(c);
      }
    }
  } else if (paletteLen > 0) {
    Serial.printf("[wisp.cfg] manualPalette blob has odd length %u; ignoring\n",
                  (unsigned)paletteLen);
  }

  Serial.printf("[wisp.cfg] loaded selZone=%d ssid='%s' pw=%s "
                "srcMode=%d manualColors=%u\n",
                selectedZone_, wifiSsid_.c_str(),
                wifiPw_.length() ? "<set>" : "<empty>",
                static_cast<int>(sourceMode_),
                (unsigned)manualPalette_.size());
}

void WispConfig::setSelectedZone(int zone) {
  if (zone < 0) {
    Serial.printf("[wisp.cfg] setSelectedZone(%d) rejected — use clearSelectedZone()\n",
                  zone);
    return;
  }
  selectedZone_ = zone;
  if (opened_) {
    prefs_.putInt(kKeyZone, zone);
  }
  Serial.printf("[wisp.cfg] selZone <= %d\n", zone);
}

void WispConfig::clearSelectedZone() {
  selectedZone_ = -1;
  if (opened_) {
    prefs_.remove(kKeyZone);
  }
  Serial.println("[wisp.cfg] selZone cleared");
}

void WispConfig::setWifi(const String& ssid, const String& pw) {
  wifiSsid_ = ssid;
  wifiPw_   = pw;
  if (opened_) {
    prefs_.putString(kKeySsid, ssid);
    prefs_.putString(kKeyPw, pw);
  }
  Serial.printf("[wisp.cfg] wifi <= ssid='%s' pw=<%u chars>\n",
                ssid.c_str(), (unsigned)pw.length());
}

void WispConfig::clearWifi() {
  wifiSsid_ = String();
  wifiPw_   = String();
  if (opened_) {
    prefs_.remove(kKeySsid);
    prefs_.remove(kKeyPw);
  }
  Serial.println("[wisp.cfg] wifi cleared");
}

void WispConfig::setSourceMode(WispSourceMode mode) {
  sourceMode_ = mode;
  if (opened_) {
    prefs_.putInt(kKeySourceMode, static_cast<int>(mode));
  }
  Serial.printf("[wisp.cfg] sourceMode <= %d\n", static_cast<int>(mode));
}

void WispConfig::setManualPalette(
    const std::vector<ManualPaletteColor>& colors) {
  // Cap at the protocol max so the persisted blob never grows past the
  // budgeted size — keeps the wispStatus JSON within CONTROL_MAX_PAYLOAD
  // regardless of what the app pushed.
  const size_t n = std::min<size_t>(colors.size(), kManualPaletteMaxColors);
  manualPalette_.assign(colors.begin(), colors.begin() + n);
  if (opened_) {
    if (n == 0) {
      prefs_.remove(kKeyManualPalette);
    } else {
      uint8_t buf[kManualPaletteMaxColors * 3];
      for (size_t i = 0; i < n; ++i) {
        buf[i * 3 + 0] = manualPalette_[i].r;
        buf[i * 3 + 1] = manualPalette_[i].g;
        buf[i * 3 + 2] = manualPalette_[i].b;
      }
      prefs_.putBytes(kKeyManualPalette, buf, n * 3);
    }
  }
  Serial.printf("[wisp.cfg] manualPalette <= %u colors\n", (unsigned)n);
}

}  // namespace wisp
