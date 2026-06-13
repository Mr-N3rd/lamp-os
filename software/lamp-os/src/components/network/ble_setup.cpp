#include "./ble_setup.hpp"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_mac.h>

#include "../../behaviors/fade_out.hpp"  // fadeOutRebootRequested flag
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
    if (pendingName.length() == 0 && pendingPassword.length() == 0) return;

    // pendingSsid is a leftover from the WiFi era and is ignored. The
    // setup-service SSID characteristic still exists for backward compat
    // but writes to it are accepted-and-discarded.
    (void)pendingSsid;
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
    Serial.printf("[ble_setup] Config persisted, fading out for reboot\n");
#endif
    // FadeOutBehavior handles the fade + ESP.restart() on its last frame.
    lamp::fadeOutRebootRequested = true;
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

  // Info characteristic: read-only, exposes MAC address for device identity.
  // Use esp_read_mac to avoid pulling in WiFi just for the MAC.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  char macStr[32];
  snprintf(macStr, sizeof(macStr), "mac=%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  NimBLECharacteristic* info =
      service->createCharacteristic(CHAR_INFO, NIMBLE_PROPERTY::READ);
  info->setValue(macStr);

  service->start();

  // Advertising start is OWNED by wifi.cpp::toApMode() and runs AFTER both
  // ble_setup and ble_control have registered their services. Calling
  // adv->start() here would lock the GATT database (ble_gatts_mutable=false)
  // before ble_control gets a chance to register its service, causing
  // ble_gatts_add_svcs() to silently fail and the control service to be
  // missing from the GATT database.

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
