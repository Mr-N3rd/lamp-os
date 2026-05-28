#include "./ble_control.hpp"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_set>

#include "../../config/config.hpp"
#include "../../behaviors/configurator.hpp"
#include "../../behaviors/fade_out.hpp"  // fadeOutRebootRequested flag
#include "../../util/color.hpp"
#include "../../lamps/standard_lamp.hpp"
#include "./bluetooth.hpp"  // for lamp::scanPausedForGattClient + BLE_GAP_SCAN_TIME_MS
#include "./nearby_lamps.hpp"
#include "./show_receiver.hpp"
#include "./wifi.hpp"

// Defined in standard_lamp.cpp. Each BLE callback does ONLY a fixed-size
// byte copy into a pending slot — zero heap allocation on Core 0. The loop
// task on Core 1 drains the slots and does all heap work (JSON parse,
// vector build, gradient construction, timestamp updates). This restores
// the single-core memory pattern the lamp used to have when the only
// transport was AsyncTCP+WebSocket (everything ran on Core 1).
void postPendingBrightness(int8_t level);
void postPendingShadeColorsJson(const char* data, size_t len);
void postPendingBaseColorsJson(const char* data, size_t len);
void postPendingKnockout(uint8_t pixel, uint8_t brightness);
void postPendingExpressionOpJson(const char* data, size_t len);
void postPendingWifiOpJson(const char* data, size_t len);
void postPendingTestActionJson(const char* data, size_t len);
void postPendingMqttOpJson(const char* data, size_t len);
void postPendingRemoteOpJson(const char* data, size_t len);
static constexpr size_t MAX_PENDING_JSON = 256;
static constexpr size_t MAX_PENDING_OP_JSON = 512;

namespace ble_control {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static NimBLEServer*         s_server      = nullptr;
static NimBLEService*        s_service     = nullptr;
static NimBLECharacteristic* s_stateNotify = nullptr;
static NimBLECharacteristic* s_wifiStateChar = nullptr;
static NimBLECharacteristic* s_lampSectionChar  = nullptr;
static NimBLECharacteristic* s_baseSectionChar  = nullptr;
static NimBLECharacteristic* s_shadeSectionChar = nullptr;
static NimBLECharacteristic* s_exprSectionChar  = nullptr;
static NimBLECharacteristic* s_homeSectionChar  = nullptr;
static NimBLECharacteristic* s_mqttSectionChar  = nullptr;
static NimBLECharacteristic* s_nearbyLampsChar  = nullptr;
static lamp::Config*         s_config      = nullptr;
static Preferences*          s_prefs       = nullptr;
static bool                  s_running     = false;

// Per-connection auth state.  Key = connection handle, value = authed.
static std::map<uint16_t, bool> s_connAuth;

// Target MTU requested on every new connection
static constexpr uint16_t TARGET_MTU = 512;

// ---------------------------------------------------------------------------
// Auth helper
// ---------------------------------------------------------------------------

static bool isAuthed(uint16_t connHandle) {
  if (s_config->lamp.password.empty()) return true;  // No password — open access
  auto it = s_connAuth.find(connHandle);
  return it != s_connAuth.end() && it->second;
}

// ---------------------------------------------------------------------------
// Server callbacks — track connections for auth state and negotiate MTU
// ---------------------------------------------------------------------------

class ControlServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    uint16_t handle = connInfo.getConnHandle();
    s_connAuth[handle] = false;

    server->setDataLen(handle, 251);
    NimBLEDevice::setMTU(TARGET_MTU);

    lamp::scanPausedForGattClient = true;
    NimBLEDevice::getScan()->stop();

    // Pause WiFi STA while a BLE client is talking to us — avoids BT/WiFi
    // coexistence stress under high GATT write rates.
    wifi::disconnect();

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] Client connected, handle=%u (scan + wifi paused)\n", handle);
#endif
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    uint16_t handle = connInfo.getConnHandle();
    s_connAuth.erase(handle);

    // Resume the central scan now that the phone is gone.
    lamp::scanPausedForGattClient = false;
    NimBLEDevice::getScan()->start(BLE_GAP_SCAN_TIME_MS);

    // Resume WiFi STA if home mode is configured.
    if (s_config && !s_config->homeMode.ssid.empty()) {
      wifi::connect(s_config->homeMode.ssid, s_config->homeMode.password);
    }

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] Client disconnected, handle=%u reason=%d (scan + wifi resumed)\n", handle, reason);
#endif
  }
};

// ---------------------------------------------------------------------------
// Auth characteristic — write-with-response
// ---------------------------------------------------------------------------

class AuthCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    uint16_t handle = connInfo.getConnHandle();
    std::string written = c->getValue();
    bool accepted = (written == s_config->lamp.password);
    s_connAuth[handle] = accepted;

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] Auth attempt handle=%u %s\n",
                  handle, accepted ? "ACCEPTED" : "REJECTED");
#endif
  }
};

// ---------------------------------------------------------------------------
// Brightness — write-without-response, single u8 0-100
// ---------------------------------------------------------------------------

class BrightnessCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty()) return;
    uint8_t level = static_cast<uint8_t>(val[0]);
    if (level > 100) level = 100;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE brightness level=%u\n", level);
#endif
    // Zero-alloc on Core 0. The drain in standard_lamp.cpp::loop() reads
    // pendingBrightness on Core 1, updates config + timestamps + strip
    // brightness all on Core 1.
    postPendingBrightness(static_cast<int8_t>(level & 0x7F));
  }
};

// ---------------------------------------------------------------------------
// Shade colors — write-without-response, JSON array of hex strings
// ---------------------------------------------------------------------------

class ShadeColorsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE shadeColors len=%u\n", (unsigned)val.size());
#endif
    // Pure memcpy. The loop drain parses the JSON on Core 1.
    postPendingShadeColorsJson(val.data(), val.size());
  }
};

// ---------------------------------------------------------------------------
// Base colors — write-without-response, JSON array of hex strings
// ---------------------------------------------------------------------------

class BaseColorsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE baseColors len=%u\n", (unsigned)val.size());
#endif
    postPendingBaseColorsJson(val.data(), val.size());
  }
};

// ---------------------------------------------------------------------------
// Base knockout — write-without-response, 2 bytes: [pixelIndex u8, brightness% u8]
// ---------------------------------------------------------------------------

class BaseKnockoutCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.size() < 2) return;
    uint8_t pixelIndex = static_cast<uint8_t>(val[0]);
    uint8_t brightness = static_cast<uint8_t>(val[1]);
    if (brightness > 100) brightness = 100;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE knockout pixel=%u brightness=%u\n", pixelIndex, brightness);
#endif
    postPendingKnockout(pixelIndex, brightness);
  }
};

class ExpressionOpCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE expressionOp len=%u\n", (unsigned)val.size());
#endif
    postPendingExpressionOpJson(val.data(), val.size());
  }
};

class MqttOpCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE mqttOp len=%u\n", (unsigned)val.size());
#endif
    postPendingMqttOpJson(val.data(), val.size());
  }
};

class WifiOpCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE wifiOp len=%u\n", (unsigned)val.size());
#endif
    postPendingWifiOpJson(val.data(), val.size());
  }
};

// ── Remote-op: forward a BLE control write to a far lamp via ESP-NOW ─────
class RemoteOpCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE remoteOp len=%u\n", (unsigned)val.size());
#endif
    postPendingRemoteOpJson(val.data(), val.size());
  }
};

// ── Nearby lamps: read+notify the unified per-transport list ────────────
// Pulls from nearbyLamps.getAll() and tags each entry with viaBle /
// viaEspNow flags reflecting whether the transport has carried at least
// one sighting in this entry's current lifetime. The list itself prunes
// after LAMP_PRUNE_TIME_MS, so stale entries disappear rather than going
// badge-less — using a separate per-flag recency window would make rows
// appear in the list with no badges between the recency cutoff and the
// prune cutoff (the user-visible "ghost row" bug).
//
// Cap JSON to MTU-3 (~509 B) — same pattern as buildWifiStateJson.
static std::string buildNearbyLampsJson() {
  auto lamps = lamp::nearbyLamps.getAll();
  // Sort by name for stable rendering.
  std::sort(lamps.begin(), lamps.end(),
            [](const lamp::NearbyLamp& a, const lamp::NearbyLamp& b) {
              return a.name < b.name;
            });
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  constexpr size_t SOFT_BUDGET = 480;
  for (const auto& p : lamps) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = p.name;
    // Use the freshest transport's timestamp as a single "lastSeen" for the
    // app's display sort / age cue.
    o["lastSeenMs"] = (p.lastSeenViaEspNowMs > p.lastSeenViaBleMs)
                          ? p.lastSeenViaEspNowMs
                          : p.lastSeenViaBleMs;
    o["viaBle"]    = (p.lastSeenViaBleMs    != 0);
    o["viaEspNow"] = (p.lastSeenViaEspNowMs != 0);
    if (p.hasMac) {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
      o["mac"] = macStr;
    }
    JsonArray sh = o["shade"].to<JsonArray>();
    sh.add(p.shadeColor.r); sh.add(p.shadeColor.g); sh.add(p.shadeColor.b); sh.add(p.shadeColor.w);
    JsonArray ba = o["base"].to<JsonArray>();
    ba.add(p.baseColor.r);  ba.add(p.baseColor.g);  ba.add(p.baseColor.b);  ba.add(p.baseColor.w);
    if (measureJson(doc) > SOFT_BUDGET) {
      arr.remove(arr.size() - 1);
      break;
    }
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

class NearbyLampsCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(buildNearbyLampsJson());
  }
};

void notifyNearbyLamps() {
  if (!s_nearbyLampsChar) return;
  auto json = buildNearbyLampsJson();
  s_nearbyLampsChar->setValue(json);
  s_nearbyLampsChar->notify();
}

static const char* wifiStateName(wifi::State s) {
  switch (s) {
    case wifi::IDLE:       return "idle";
    case wifi::SCANNING:   return "scanning";
    case wifi::CONNECTING: return "connecting";
    case wifi::CONNECTED:  return "connected";
    case wifi::FAILED:     return "failed";
  }
  return "unknown";
}

static std::string buildWifiStateJson(bool includeScanResults) {
  JsonDocument doc;
  doc["state"] = wifiStateName(wifi::state());
  doc["ssid"] = wifi::currentSsid();
  doc["ip"] = wifi::currentIp();
  if (!wifi::lastError().empty()) {
    doc["lastError"] = wifi::lastError();
  }
  if (includeScanResults) {
    auto results = wifi::consumeScanResults();
    if (!results.empty()) {
      // Sort strongest first so the trimmed list keeps the most useful entries.
      std::sort(results.begin(), results.end(),
                [](const wifi::ScanResult& a, const wifi::ScanResult& b) {
                  return a.rssi > b.rssi;
                });
      // Drop duplicate SSIDs (multiple BSSIDs per network are common).
      std::unordered_set<std::string> seen;
      JsonArray arr = doc["scanResults"].to<JsonArray>();
      // BLE notify payload is capped at MTU-3 (~509 B). Stop adding entries
      // once the serialized doc would exceed a soft budget so the notify
      // doesn't get rejected by NimBLE with `val > max`.
      constexpr size_t SOFT_BUDGET = 480;
      for (const auto& r : results) {
        if (r.ssid.empty()) continue;
        if (!seen.insert(r.ssid).second) continue;
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = r.ssid;
        o["rssi"] = r.rssi;
        o["encrypted"] = r.encrypted;
        if (measureJson(doc) > SOFT_BUDGET) {
          arr.remove(arr.size() - 1);
          break;
        }
      }
    }
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

class WifiStateCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    auto json = buildWifiStateJson(true);
    c->setValue(json);
  }
};

void notifyWifiState() {
  if (!s_wifiStateChar) return;
  auto json = buildWifiStateJson(true);
  s_wifiStateChar->setValue(json);
  s_wifiStateChar->notify();
}

class ExpressionTestCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE expressionTest len=%u\n", (unsigned)val.size());
#endif
    // Empty payload is the "test complete" signal — must reach the drain.
    postPendingTestActionJson(val.data(), val.size());
  }
};

// ---------------------------------------------------------------------------
// Settings blob — read + write-with-response
//   Read:  returns full config JSON (no auth required — app needs it to
//          determine if a password exists before it can auth).
//   Write: replaces config and persists to NVS; then reboots (same semantics
//          as HTTP PUT /settings).
// ---------------------------------------------------------------------------

class SettingsBlobCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    JsonDocument doc = s_config->asJsonDocument();
    String json;
    serializeJson(doc, json);
    c->setValue(json.c_str());
  }

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] settings_blob write: not authed\n");
#endif
      return;
    }

    std::string json = c->getValue();
    if (json.empty()) return;

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE settingsBlob len=%u\n", (unsigned)json.size());
#endif

    s_prefs->begin("lamp", false);
    size_t written = s_prefs->putString("cfg", json.c_str());
    s_prefs->end();

    if (written > 0) {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] settings_blob: persisted %zu bytes, fading out for reboot\n", written);
#endif
      notifyStateChange();
      // Signal FadeOutBehavior to play the fade-to-black animation and then
      // call ESP.restart() on its last frame. Better UX than abrupt reboot,
      // and gives any in-flight GATT ack time to land.
      lamp::fadeOutRebootRequested = true;
    } else {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] settings_blob: putString failed\n");
#endif
    }
  }
};

// ---------------------------------------------------------------------------
// Per-section settings reads — split the old settings_blob into 5 smaller
// characteristics so each stays well under MTU and can grow independently.
// ---------------------------------------------------------------------------

class LampSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asLampJson().c_str());
  }
};

class BaseSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asBaseJson().c_str());
  }
};

class ShadeSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asShadeJson().c_str());
  }
};

class ExprSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asExpressionsJson().c_str());
  }
};

class MqttSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asMqttJson().c_str());
  }
};

class HomeSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asHomeModeJson().c_str());
  }
};

static void notifySection(NimBLECharacteristic* c, const String& json) {
  if (!c) return;
  c->setValue(json.c_str());
  c->notify();
}

void notifyLampSection()        { notifySection(s_lampSectionChar,  s_config->asLampJson());        }
void notifyBaseSection()        { notifySection(s_baseSectionChar,  s_config->asBaseJson());        }
void notifyShadeSection()       { notifySection(s_shadeSectionChar, s_config->asShadeJson());       }
void notifyExpressionsSection() { notifySection(s_exprSectionChar,  s_config->asExpressionsJson()); }
void notifyHomeModeSection()    { notifySection(s_homeSectionChar,  s_config->asHomeModeJson());    }
void notifyMqttSection()        { notifySection(s_mqttSectionChar,  s_config->asMqttJson());        }

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void notifyStateChange() {
  if (!s_stateNotify) return;
  // Minimal payload — clients that need details re-fetch via settings_blob.
  s_stateNotify->setValue("{}");
  s_stateNotify->notify();
}

void start(lamp::Config* config, Preferences* prefs) {
  if (s_running) return;

  s_config = config;
  s_prefs  = prefs;

  if (!NimBLEDevice::isInitialized()) {
    NimBLEDevice::init(config->lamp.name.substr(0, 12));
  }

  // NimBLE supports only one server per device.  createServer() returns the
  // existing instance on subsequent calls, so this is safe to call even if
  // ble_setup already created a server.
  s_server = NimBLEDevice::createServer();
  // Pass deleteCallbacks=false: the callbacks object lives for the process
  // lifetime and must not be freed by NimBLE.
  s_server->setCallbacks(new ControlServerCallbacks(), false);

  // NimBLE 2.x default is FALSE — advertising does NOT auto-restart after a
  // client disconnects (per CHANGELOG). Without this, the first phone disconnect
  // makes the lamp permanently undiscoverable until reboot.
  s_server->advertiseOnDisconnect(true);

  s_service = s_server->createService(SERVICE_UUID);

  // Auth — write-with-response so the app receives a GATT ack.
  // WRITE_ENC: carries the lamp password — require a pair/bond first so
  // the password is sent over an encrypted link, not in cleartext.
  s_service->createCharacteristic(CHAR_AUTH,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC)
      ->setCallbacks(new AuthCallback());

  s_service->createCharacteristic(CHAR_BRIGHTNESS, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new BrightnessCallback());
  s_service->createCharacteristic(CHAR_SHADE_COLORS, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ShadeColorsCallback());
  s_service->createCharacteristic(CHAR_BASE_COLORS, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new BaseColorsCallback());
  s_service->createCharacteristic(CHAR_BASE_KNOCKOUT, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new BaseKnockoutCallback());
  s_service->createCharacteristic(CHAR_EXPRESSION_TEST, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ExpressionTestCallback());
  s_service->createCharacteristic(CHAR_EXPRESSION_OP, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ExpressionOpCallback());
  // WRITE_ENC: carries the user's home WiFi password — must not be sniffable.
  s_service->createCharacteristic(CHAR_WIFI_OP,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC)
      ->setCallbacks(new WifiOpCallback());
  // WRITE_ENC: carries the broker password.
  s_service->createCharacteristic(CHAR_MQTT_OP,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC)
      ->setCallbacks(new MqttOpCallback());
  s_wifiStateChar = s_service->createCharacteristic(
      CHAR_WIFI_STATE,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_wifiStateChar->setCallbacks(new WifiStateCallback());
  s_wifiStateChar->setValue(buildWifiStateJson(false));

  // Per-section settings characteristics — read + notify. Each onRead refreshes
  // the cached value from live config. NimBLE returns the cached value on a
  // GATT read, so we also seed the initial values here to handle the first read
  // before any onRead fires.
  s_lampSectionChar = s_service->createCharacteristic(
      CHAR_LAMP_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_lampSectionChar->setCallbacks(new LampSectionCallback());
  s_lampSectionChar->setValue(s_config->asLampJson().c_str());

  s_baseSectionChar = s_service->createCharacteristic(
      CHAR_BASE_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_baseSectionChar->setCallbacks(new BaseSectionCallback());
  s_baseSectionChar->setValue(s_config->asBaseJson().c_str());

  s_shadeSectionChar = s_service->createCharacteristic(
      CHAR_SHADE_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_shadeSectionChar->setCallbacks(new ShadeSectionCallback());
  s_shadeSectionChar->setValue(s_config->asShadeJson().c_str());

  s_exprSectionChar = s_service->createCharacteristic(
      CHAR_EXPR_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_exprSectionChar->setCallbacks(new ExprSectionCallback());
  s_exprSectionChar->setValue(s_config->asExpressionsJson().c_str());

  s_homeSectionChar = s_service->createCharacteristic(
      CHAR_HOME_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_homeSectionChar->setCallbacks(new HomeSectionCallback());
  s_homeSectionChar->setValue(s_config->asHomeModeJson().c_str());

  s_mqttSectionChar = s_service->createCharacteristic(
      CHAR_MQTT_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_mqttSectionChar->setCallbacks(new MqttSectionCallback());
  s_mqttSectionChar->setValue(s_config->asMqttJson().c_str());

  s_nearbyLampsChar = s_service->createCharacteristic(
      CHAR_NEARBY_LAMPS, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_nearbyLampsChar->setCallbacks(new NearbyLampsCallback());
  s_nearbyLampsChar->setValue(buildNearbyLampsJson());

  // WRITE_ENC: remote-op payloads can carry credentials in settings/mqtt
  // forwarding. First call after pairing triggers the OS pair dialog.
  s_service->createCharacteristic(CHAR_REMOTE_OP,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC)
      ->setCallbacks(new RemoteOpCallback());

  // Settings blob — write-only. Reads now go through the per-section
  // characteristics (CHAR_LAMP_SECTION etc.), each well under MTU. The
  // single-blob read path was dropped because the full config grew past 512
  // bytes after homeMode was added; see commit da5d4d9.
  // WRITE_ENC: full config save embeds the lamp password.
  s_service->createCharacteristic(CHAR_SETTINGS_BLOB,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC)
      ->setCallbacks(new SettingsBlobCallback());

  // State notify — notify only; no write/read needed
  s_stateNotify = s_service->createCharacteristic(CHAR_STATE_NOTIFY,
                                                  NIMBLE_PROPERTY::NOTIFY);
  s_stateNotify->setValue("{}");

  s_service->start();

  // Don't touch advertising — BluetoothComponent::begin() already configures
  // the advertiser as connectable (BLE_GAP_CONN_MODE_UND) with the color-sync
  // manufacturer data the app scans for. The control GATT service attaches to
  // the GATT server and is discovered AFTER connection, not advertised in the
  // packet (a 128-bit service UUID would overflow the 31-byte adv limit).

  s_running = true;

#ifdef LAMP_DEBUG
  Serial.printf("[ble_control] GATT control service started\n");
#endif
}

void stop() {
  if (!s_running) return;

  NimBLEDevice::getAdvertising()->stop();

  if (s_server && s_service) {
    s_server->removeService(s_service, true);
    s_service     = nullptr;
    s_stateNotify = nullptr;
    s_wifiStateChar = nullptr;
    s_lampSectionChar = nullptr;
    s_baseSectionChar = nullptr;
    s_shadeSectionChar = nullptr;
    s_exprSectionChar = nullptr;
    s_homeSectionChar = nullptr;
    s_mqttSectionChar = nullptr;
    s_nearbyLampsChar = nullptr;
  }

  s_server  = nullptr;
  s_running = false;
  s_connAuth.clear();

#ifdef LAMP_DEBUG
  Serial.printf("[ble_control] GATT control service stopped\n");
#endif
}

bool isRunning() { return s_running; }

}  // namespace ble_control
