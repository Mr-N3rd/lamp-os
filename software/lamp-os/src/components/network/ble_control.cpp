#include "./ble_control.hpp"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <map>
#include <string>

#include "../../config/config.hpp"
#include "../../behaviors/configurator.hpp"
#include "../../behaviors/fade_out.hpp"  // fadeOutRebootRequested flag
#include "../../util/color.hpp"
#include "../../lamps/standard_lamp.hpp"
#include "./bluetooth.hpp"  // for lamp::scanPausedForGattClient + BLE_GAP_SCAN_TIME_MS

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
static constexpr size_t MAX_PENDING_JSON = 256;

namespace ble_control {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static NimBLEServer*         s_server      = nullptr;
static NimBLEService*        s_service     = nullptr;
static NimBLECharacteristic* s_stateNotify = nullptr;
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

    // Request a higher MTU so color JSON and settings blob fit in fewer packets.
    // BT Core spec caps LL data length (DLE) at 251 bytes; passing TARGET_MTU
    // (=512) here makes NimBLE return BLE_HS_EINVAL ("Set data length error: 3"
    // in the serial log). MTU max is 517, so setMTU still uses TARGET_MTU.
    server->setDataLen(handle, 251);
    NimBLEDevice::setMTU(TARGET_MTU);

    // Pause the lamp's BLE central scan while a phone is connected. The
    // constant scan-callback churn on the NimBLE host task contends with
    // high-rate write-without-response traffic and was crashing the lamp
    // under sustained color drag. The flag also makes onScanEnd skip its
    // auto-restart, so the scan stays down until disconnect.
    lamp::scanPausedForGattClient = true;
    NimBLEDevice::getScan()->stop();

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] Client connected, handle=%u (scan paused)\n", handle);
#endif
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    uint16_t handle = connInfo.getConnHandle();
    s_connAuth.erase(handle);

    // Resume the central scan now that the phone is gone.
    lamp::scanPausedForGattClient = false;
    NimBLEDevice::getScan()->start(BLE_GAP_SCAN_TIME_MS);

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] Client disconnected, handle=%u reason=%d (scan resumed)\n", handle, reason);
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
    postPendingKnockout(pixelIndex, brightness);
  }
};

class ExpressionTestCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;

    std::string value = c->getValue();
    if (value.empty()) {
      JsonDocument doc;
      doc["a"] = "test_expression_complete";
      dispatchLampAction(doc, millis());
      return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, value.c_str(), value.size());
    const char* action = err ? nullptr : doc["a"].as<const char*>();
    if (action && *action) {
      dispatchLampAction(doc, millis());
      return;
    }

    doc.clear();
    if (value == "complete") {
      doc["a"] = "test_expression_complete";
    } else {
      doc["a"] = "test_expression";
      doc["type"] = value;
    }
    dispatchLampAction(doc, millis());
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

  // Auth — write-with-response so the app receives a GATT ack
  s_service->createCharacteristic(CHAR_AUTH, NIMBLE_PROPERTY::WRITE)
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

  // Settings blob — read + write-with-response
  // Important: NimBLE returns the previously-set value on reads; the onRead
  // callback runs alongside but the response payload is the captured value.
  // So we must seed the value here at creation time AND keep it refreshed
  // on each onRead so subsequent reads see updated config.
  NimBLECharacteristic* settingsBlobChar = s_service->createCharacteristic(
      CHAR_SETTINGS_BLOB,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  settingsBlobChar->setCallbacks(new SettingsBlobCallback());
  {
    JsonDocument doc = s_config->asJsonDocument();
    String json;
    serializeJson(doc, json);
    settingsBlobChar->setValue(json.c_str());
    Serial.printf("[ble_control] settings_blob initial value set, %d bytes\n",
                  (int)json.length());
  }

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
