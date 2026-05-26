#include "./ble_control.hpp"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <map>
#include <string>

#include "../../config/config.hpp"
#include "../../lamps/standard_lamp.hpp"

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
    server->setDataLen(handle, TARGET_MTU);
    NimBLEDevice::setMTU(TARGET_MTU);

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] Client connected, handle=%u\n", handle);
#endif
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    uint16_t handle = connInfo.getConnHandle();
    s_connAuth.erase(handle);

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] Client disconnected, handle=%u reason=%d\n", handle, reason);
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
    if (!isAuthed(connInfo.getConnHandle())) {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] brightness: write rejected (not authed)\n");
#endif
      return;
    }

    std::string val = c->getValue();
    if (val.empty()) return;

    uint8_t level = static_cast<uint8_t>(val[0]);
    if (level > 100) level = 100;

    JsonDocument doc;
    doc["a"] = "bright";
    doc["v"] = level;
    dispatchLampAction(doc, millis());

    notifyStateChange();
  }
};

// ---------------------------------------------------------------------------
// Shade colors — write-without-response, JSON array of hex strings
// ---------------------------------------------------------------------------

class ShadeColorsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;

    std::string json = c->getValue();
    if (json.empty()) return;

    // Parse the incoming color array and forward as an action document
    JsonDocument colors;
    DeserializationError err = deserializeJson(colors, json);
    if (err) {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] shade_colors: bad JSON: %s\n", err.c_str());
#endif
      return;
    }

    JsonDocument doc;
    doc["a"] = "shade";
    doc["c"] = colors.as<JsonArray>();
    dispatchLampAction(doc, millis());
  }
};

// ---------------------------------------------------------------------------
// Base colors — write-without-response, JSON array of hex strings
// ---------------------------------------------------------------------------

class BaseColorsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;

    std::string json = c->getValue();
    if (json.empty()) return;

    JsonDocument colors;
    DeserializationError err = deserializeJson(colors, json);
    if (err) {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] base_colors: bad JSON: %s\n", err.c_str());
#endif
      return;
    }

    JsonDocument doc;
    doc["a"] = "base";
    doc["c"] = colors.as<JsonArray>();
    dispatchLampAction(doc, millis());
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

    JsonDocument doc;
    doc["a"] = "knockout";
    doc["p"] = pixelIndex;
    doc["b"] = brightness;
    dispatchLampAction(doc, millis());
  }
};

// ---------------------------------------------------------------------------
// Expression test — write-with-response
//   Non-empty string  → start preview of that expression type.
//   Empty string or "complete" → end preview (restore configurator colors).
// ---------------------------------------------------------------------------

class ExpressionTestCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;

    std::string type = c->getValue();

    JsonDocument doc;
    if (type.empty() || type == "complete") {
      doc["a"] = "test_expression_complete";
    } else {
      doc["a"] = "test_expression";
      doc["type"] = type;
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
      Serial.printf("[ble_control] settings_blob: persisted %zu bytes, rebooting\n", written);
#endif
      notifyStateChange();
      delay(200);
      ESP.restart();
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

  s_service = s_server->createService(SERVICE_UUID);

  // Auth — write-with-response so the app receives a GATT ack
  s_service->createCharacteristic(CHAR_AUTH, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new AuthCallback());

  // Brightness — write-without-response for minimum latency during drag
  s_service->createCharacteristic(CHAR_BRIGHTNESS, NIMBLE_PROPERTY::WRITE_NR)
      ->setCallbacks(new BrightnessCallback());

  // Shade colors — write-without-response
  s_service->createCharacteristic(CHAR_SHADE_COLORS, NIMBLE_PROPERTY::WRITE_NR)
      ->setCallbacks(new ShadeColorsCallback());

  // Base colors — write-without-response
  s_service->createCharacteristic(CHAR_BASE_COLORS, NIMBLE_PROPERTY::WRITE_NR)
      ->setCallbacks(new BaseColorsCallback());

  // Base knockout — write-without-response
  s_service->createCharacteristic(CHAR_BASE_KNOCKOUT, NIMBLE_PROPERTY::WRITE_NR)
      ->setCallbacks(new BaseKnockoutCallback());

  // Expression test — write-with-response
  s_service->createCharacteristic(CHAR_EXPRESSION_TEST, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ExpressionTestCallback());

  // Settings blob — read + write-with-response
  s_service->createCharacteristic(CHAR_SETTINGS_BLOB,
                                  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new SettingsBlobCallback());

  // State notify — notify only; no write/read needed
  s_stateNotify = s_service->createCharacteristic(CHAR_STATE_NOTIFY,
                                                  NIMBLE_PROPERTY::NOTIFY);
  s_stateNotify->setValue("{}");

  s_service->start();

  // Add the control service UUID to advertising so the app can filter by it.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  // Make the device connectable (undirected connectable advertising mode).
  adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  adv->start();

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
