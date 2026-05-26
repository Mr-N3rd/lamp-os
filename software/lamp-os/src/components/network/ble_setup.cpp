#include "./ble_setup.hpp"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

#include "../../config/config.hpp"

namespace ble_setup {

static NimBLEServer*  server  = nullptr;
static NimBLEService* service = nullptr;
static bool running = false;

// Staging area for values written by the mobile app before apply
static String pendingSsid;
static String pendingPassword;
static String pendingName;

// Pointers supplied at start() — valid for the lifetime of the service
static lamp::Config*  s_config = nullptr;
static Preferences*   s_prefs  = nullptr;

// ---------------------------------------------------------------------------
// Characteristic callbacks
// ---------------------------------------------------------------------------

/**
 * Generic write callback: stores the written value into a String target.
 */
class FieldCallback : public NimBLECharacteristicCallbacks {
 public:
  explicit FieldCallback(String* target) : target_(target) {}

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    *target_ = String(c->getValue().c_str());
  }

 private:
  String* target_;
};

/**
 * Apply callback: persists staged values to NVS and reboots.
 *
 * SSID (pendingSsid) → config->lamp.homeModeSSID
 *   The "WiFi SSID" in this lamp's architecture is the home-mode detection
 *   network; no STA join occurs — the lamp always runs as SoftAP.
 *
 * Password (pendingPassword) → config->lamp.password
 *   Protects the lamp's HTTP/WebSocket API.
 *
 * Name (pendingName) → config->lamp.name
 *   Identifies the lamp in mDNS and BLE advertising.
 *
 * Persistence follows the pattern in wifi.cpp PUT /settings: serialize the
 * full config to JSON and write to Preferences key "cfg" in namespace "lamp".
 */
class ApplyCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (pendingSsid.length() == 0 && pendingName.length() == 0) return;

    if (pendingSsid.length() > 0) {
      s_config->lamp.homeModeSSID = std::string(pendingSsid.c_str());
    }
    if (pendingPassword.length() > 0) {
      s_config->lamp.password = std::string(pendingPassword.c_str());
    }
    if (pendingName.length() > 0) {
      s_config->lamp.name = std::string(pendingName.c_str());
    }

    // Serialize and persist — same mechanism as PUT /settings in wifi.cpp
    JsonDocument doc = s_config->asJsonDocument();
    String json;
    serializeJson(doc, json);

    s_prefs->begin("lamp", false);
    s_prefs->putString("cfg", json);
    s_prefs->end();

#ifdef LAMP_DEBUG
    Serial.printf("[ble_setup] Config persisted, rebooting\n");
#endif

    delay(200);
    ESP.restart();
  }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void start(lamp::Config* config, Preferences* prefs) {
  if (running) return;

  s_config = config;
  s_prefs  = prefs;

  // NimBLEDevice::init() is expected to have been called already by
  // BluetoothComponent::begin(). Guard defensively in case it wasn't.
  if (!NimBLEDevice::isInitialized()) {
    NimBLEDevice::init(config->lamp.name.substr(0, 12));
  }

  server  = NimBLEDevice::createServer();
  service = server->createService(SERVICE_UUID);

  service->createCharacteristic(CHAR_SSID, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new FieldCallback(&pendingSsid));

  service->createCharacteristic(CHAR_PASSWORD, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new FieldCallback(&pendingPassword));

  service->createCharacteristic(CHAR_NAME, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new FieldCallback(&pendingName));

  service->createCharacteristic(CHAR_APPLY, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ApplyCallback());

  // Info characteristic: read-only, exposes MAC address for device identity
  NimBLECharacteristic* info =
      service->createCharacteristic(CHAR_INFO, NIMBLE_PROPERTY::READ);
  info->setValue((String("mac=") + WiFi.macAddress()).c_str());

  service->start();

  // Add setup service UUID to the existing advertising payload so the mobile
  // app can filter by service UUID during discovery.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  // Re-start advertising to pick up the new UUID (no-op if already running
  // but ensures the updated data is broadcast).
  adv->start();

  running = true;

#ifdef LAMP_DEBUG
  Serial.printf("[ble_setup] GATT setup service started\n");
#endif
}

void stop() {
  if (!running) return;

  NimBLEDevice::getAdvertising()->stop();

  if (server && service) {
    server->removeService(service, true);
    service = nullptr;
  }

  server  = nullptr;
  running = false;

#ifdef LAMP_DEBUG
  Serial.printf("[ble_setup] GATT setup service stopped\n");
#endif
}

bool isRunning() { return running; }

}  // namespace ble_setup
