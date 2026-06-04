#include "WispConfig.h"

namespace wisp {

namespace {
constexpr const char* kNamespace = "wisp";
constexpr const char* kKeyZone   = "selZone";
constexpr const char* kKeySsid   = "wifiSsid";
constexpr const char* kKeyPw     = "wifiPw";
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

  Serial.printf("[wisp.cfg] loaded selZone=%d ssid='%s' pw=%s\n",
                selectedZone_, wifiSsid_.c_str(),
                wifiPw_.length() ? "<set>" : "<empty>");
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

}  // namespace wisp
