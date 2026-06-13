/**
 *  Lamp Bluetooth Management. Pure-BLE v1 — no WiFi, no stage mode, no ArtNet.
 */
#include "./bluetooth.hpp"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <string>
#include <vector>

#include "../../config/config.hpp"
#include "../../util/color.hpp"
#include "./ble_control.hpp"
#include "./ble_setup.hpp"
#include "./nearby_lamps.hpp"

namespace lamp {

// Set true by ble_control on GATT connect; suppresses scan-restart in
// onScanEnd. Cleared on disconnect.
volatile bool scanPausedForGattClient = false;

class ScanCallbacks : public NimBLEScanCallbacks {
  bool isLamp(std::string data) {
    return (data.length() == 8 &&
            data[0] == (BLE_LAMP_MAGIC_NUMBER & 0xff) &&
            data[1] == ((BLE_LAMP_MAGIC_NUMBER >> 8) & 0xff));
  };

  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override {
    if (!advertisedDevice->haveName() || !advertisedDevice->haveManufacturerData()) return;
    if (advertisedDevice->getRSSI() <= BLE_MINIMUM_RSSI_VALUE) return;
    std::string data = advertisedDevice->getManufacturerData();
    if (!isLamp(data)) return;

    nearbyLamps.addOrUpdateFromBle(
        advertisedDevice->getName(),
        Color(data[2], data[3], data[4], 0),
        Color(data[5], data[6], data[7], 0));
  };

  void onScanEnd(const NimBLEScanResults &results, int reason) override {
    nearbyLamps.prune(LAMP_PRUNE_TIME_MS);
    // Skip restart while a phone is using the GATT control service.
    // ble_control resumes the scan on disconnect.
    if (!scanPausedForGattClient) {
      NimBLEDevice::getScan()->start(BLE_GAP_SCAN_TIME_MS);
    }
  }
} scanCallbacks;

BluetoothComponent::BluetoothComponent() {};

void BluetoothComponent::begin(std::string name, Color inBaseColor,
                               Color inShadeColor) {
#ifdef LAMP_DEBUG
  Serial.printf("Starting Bluetooth Async Client\n");
#endif
  NimBLEDevice::init(name.substr(0, 12));
  NimBLEDevice::setPower(BLE_POWER_LEVEL);

  // LE Secure Connections + Just-Works bonding remain enabled, but the
  // link layer no longer forces encryption on any characteristic — see
  // `app-layer crypto` below. Sensitive writes (CHAR_AUTH, CHAR_WIFI_OP,
  // CHAR_MQTT_OP, CHAR_REMOTE_OP, CHAR_SETTINGS_BLOB) accept an
  // app-layer AES-GCM frame keyed off the lamp password via
  // `lamp::crypto`; legacy plaintext writes still work for the
  // webapp/old clients. The OS will not pop a pair dialog on any
  // write. Phones bonded under the old WRITE_ENC scheme still
  // re-encrypt silently because their bond record is still valid;
  // fresh phones simply skip the bond altogether.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true,
                                /*mitm=*/false,
                                /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEScan *pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks);
  pScan->setInterval(BLE_GAP_ADV_INTERVAL_MS);
  pScan->setWindow(BLE_GAP_SCAN_WINDOW_MS);
  pScan->setActiveScan(true);
  pScan->start(BLE_GAP_SCAN_TIME_MS);

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName(name);
  pAdvertising->enableScanResponse(true);
  std::vector<unsigned char> data{
      static_cast<unsigned char>(BLE_LAMP_MAGIC_NUMBER & 0xff),
      static_cast<unsigned char>((BLE_LAMP_MAGIC_NUMBER >> 8) & 0xff),
      static_cast<unsigned char>(inBaseColor.r),
      static_cast<unsigned char>(inBaseColor.g),
      static_cast<unsigned char>(inBaseColor.b),
      static_cast<unsigned char>(inShadeColor.r),
      static_cast<unsigned char>(inShadeColor.g),
      static_cast<unsigned char>(inShadeColor.b),
  };
  pAdvertising->setManufacturerData(data);
  pAdvertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  pAdvertising->setMinInterval(BLE_ADVERTISING_INTERVAL_MIN);
  pAdvertising->setMaxInterval(BLE_ADVERTISING_INTERVAL_MAX);
  // Advertising start is deferred to activateGattServices() — NimBLE's
  // GATT database is frozen once advertising starts.
  Serial.printf("[ble] advertising configured for name=%s (deferred start)\n",
                name.c_str());
};

void BluetoothComponent::activateGattServices(Config* cfg, Preferences* prefs) {
  // NimBLE's ble_gatts_mutable() returns false if any GAP procedure is
  // active, and ble_gatts_add_svcs() silently drops services if so. Pause
  // the central scan while registering services + starting advertising.
  NimBLEScan* scan = NimBLEDevice::getScan();
  bool wasScanning = scan->isScanning();
  if (wasScanning) {
    scan->stop();
    Serial.printf("[ble] stopped central scan for GATT registration\n");
  }

  ble_setup::start(cfg, prefs);
  ble_control::start(cfg, prefs);

  NimBLEDevice::getServer()->start();
  Serial.printf("[ble] server.start() done (GATT services registered)\n");

  bool advStarted = NimBLEDevice::getAdvertising()->start();
  Serial.printf("[ble] advertising started=%d\n", advStarted);

  if (wasScanning) {
    scan->start(0);  // 0 = continuous
    Serial.printf("[ble] central scan restarted\n");
  }
}

}  // namespace lamp
