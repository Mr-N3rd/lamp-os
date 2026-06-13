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

// Cached manufacturer-data vector. `begin()` populates it; `setMeshState()`
// mutates the mesh byte in place and re-applies. Lives at file scope so
// the NimBLE advertising peripheral can read it back even though
// NimBLEAdvertising doesn't expose a getter for the previously-set value
// in this version of the library.
static std::vector<unsigned char> s_advertisementData;
// Cached name so `setMeshState` can rebuild the full advertisement payload
// from scratch (see comments there — NimBLE's setManufacturerData
// *appends* rather than replaces, so we have to rebuild via
// setAdvertisementData to keep the payload from growing forever).
static std::string s_advertisementName;

static void applyAdvertisementPayload(NimBLEAdvertising* adv,
                                      const std::string& name,
                                      const std::vector<unsigned char>& mfg) {
  // Build the advertisement payload from scratch and atomically replace
  // it via `setAdvertisementData`. The high-level setManufacturerData /
  // setName helpers on NimBLEAdvertising both `addData` to an internal
  // accumulator (NimBLEAdvertising.cpp ~ line 270) — a second call
  // doesn't replace the previous mfg field, it appends a SECOND mfg
  // field after it, which trips the 31-byte limit and falls back to the
  // scan-response packet (firing the "Data length exceeded" warning).
  NimBLEAdvertisementData data;
  data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  data.setName(name, /*isComplete=*/true);
  data.setManufacturerData(mfg);
  adv->setAdvertisementData(data);
}

class ScanCallbacks : public NimBLEScanCallbacks {
  bool isLamp(std::string data) {
    // v1 payloads are 8 bytes (magic16 + baseRGB + shadeRGB).
    // v2 payloads are 6 bytes (magic16 + baseRGB + meshFlag) — shade was
    // dropped to fit inside NimBLE's adv-data limit once we added the
    // mesh-state byte. Accept both so v1 lamps still register as peers.
    return ((data.length() == 6 || data.length() == 8) &&
            data[0] == (BLE_LAMP_MAGIC_NUMBER & 0xff) &&
            data[1] == ((BLE_LAMP_MAGIC_NUMBER >> 8) & 0xff));
  };

  void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override {
    if (!advertisedDevice->haveName() || !advertisedDevice->haveManufacturerData()) return;
    if (advertisedDevice->getRSSI() <= BLE_MINIMUM_RSSI_VALUE) return;
    std::string data = advertisedDevice->getManufacturerData();
    if (!isLamp(data)) return;

    // For v2 (6 bytes) we only carry base; shade defaults to black on
    // peers' nearbyLamps store.
    Color base(data[2], data[3], data[4], 0);
    Color shade = data.length() == 8
                      ? Color(data[5], data[6], data[7], 0)
                      : Color(0, 0, 0, 0);
    nearbyLamps.addOrUpdateFromBle(advertisedDevice->getName(), base, shade);
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
  s_advertisementName = name;
  // Scan response stays off — we build the entire payload into the main
  // advertisement packet via setAdvertisementData below, which gives us
  // deterministic control over what goes on the wire.
  pAdvertising->enableScanResponse(false);
  // v2 payload: magic16(2), baseRGB(3), meshFlag(1) = 6 bytes total.
  // Shade was dropped from v1's 8-byte payload — adding the mesh byte
  // pushed the total adv past NimBLE's "Data length exceeded" cap.
  // baseRGB alone is enough for the app's factory-default detection
  // (the default purple 0x300783 is distinctive on its own).
  //
  // Mesh byte starts at 0 (off-mesh) and flips once `setMeshState(true)`
  // is called from the main loop when peers come into range.
  (void)inShadeColor;
  s_advertisementData = {
      static_cast<unsigned char>(BLE_LAMP_MAGIC_NUMBER & 0xff),
      static_cast<unsigned char>((BLE_LAMP_MAGIC_NUMBER >> 8) & 0xff),
      static_cast<unsigned char>(inBaseColor.r),
      static_cast<unsigned char>(inBaseColor.g),
      static_cast<unsigned char>(inBaseColor.b),
      0x00,
  };
  applyAdvertisementPayload(pAdvertising, s_advertisementName,
                            s_advertisementData);
  pAdvertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  pAdvertising->setMinInterval(BLE_ADVERTISING_INTERVAL_MIN);
  pAdvertising->setMaxInterval(BLE_ADVERTISING_INTERVAL_MAX);
  // Advertising start is deferred to activateGattServices() — NimBLE's
  // GATT database is frozen once advertising starts.
  Serial.printf("[ble] advertising configured for name=%s (deferred start)\n",
                name.c_str());
};

void BluetoothComponent::setMeshState(bool onMesh) {
  // Re-apply the manufacturer data only when the flag actually flips —
  // setManufacturerData() is cheap but it churns the active advertisement
  // packet, so we avoid doing it every loop tick.
  static bool s_lastMeshState = false;
  static bool s_meshStateInitialized = false;
  if (s_meshStateInitialized && s_lastMeshState == onMesh) return;
  s_lastMeshState = onMesh;
  s_meshStateInitialized = true;

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  if (pAdvertising == nullptr) return;
  if (s_advertisementData.size() < 6) {
    // begin() hasn't run yet — nothing meaningful to re-apply.
    return;
  }
  // Mesh byte is at index 5 (after magic16 + baseRGB(3)).
  s_advertisementData[5] = onMesh ? 0x01 : 0x00;
  applyAdvertisementPayload(pAdvertising, s_advertisementName,
                            s_advertisementData);
#ifdef LAMP_DEBUG
  Serial.printf("[ble] mesh advertisement byte set to %u\n",
                (unsigned)s_advertisementData[5]);
#endif
}

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
