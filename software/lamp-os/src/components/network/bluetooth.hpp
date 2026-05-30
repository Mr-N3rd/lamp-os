#ifndef LAMP_COMPONENTS_NETWORK_BLUETOOTH_H
#define LAMP_COMPONENTS_NETWORK_BLUETOOTH_H

#include <string>

#include "../../config/config.hpp"

// Lamp manufacturer identifier
#define BLE_LAMP_MAGIC_NUMBER 42069

// Scan every INTERVAL for WINDOW
#define BLE_GAP_SCAN_INTERVAL_MS 400
#define BLE_GAP_SCAN_WINDOW_MS 15

// Advertise every INTERVAL
#define BLE_GAP_ADV_INTERVAL_MS 1000

// Scan time
#define BLE_GAP_SCAN_TIME_MS 1000

// Advertising intervals (BLE units of 0.625 ms). Lamp is mains-powered so
// no reason not to advertise fast.
#define BLE_ADVERTISING_INTERVAL_MIN 48
#define BLE_ADVERTISING_INTERVAL_MAX 96

// Tx power level in DB
// @see platformio build flag MYNEWT_VAL_BLE_LL_TX_PWR_DBM as they must match
#define BLE_POWER_LEVEL 4

// Minimum RSSI to be included/updated in the lamp pool
#define BLE_MINIMUM_RSSI_VALUE -94

namespace lamp {
// Set by ble_control on GATT connect/disconnect. While true, the scan
// auto-restart in onScanEnd is suppressed. Defined in bluetooth.cpp.
extern volatile bool scanPausedForGattClient;

/**
 * @brief Entrypoint class to advertise and track lamps by Bluetooth LE
 */
class BluetoothComponent {
 public:
  BluetoothComponent();

  /**
   * @brief initialize bluetooth with the user's lamp name and colors
   * @param [in] name max. 13 character string representing the lamp's name
   * @param [in] inBaseColor the base color RGB value. W is ommitted
   * @param [in] inShadeColor the shade color RGB value. W is ommitted
   */
  void begin(std::string name, Color inBaseColor, Color inShadeColor);

  /**
   * @brief register the GATT setup + control services, start the GATT server,
   * start advertising. Must be called after begin() and after any preferences
   * loaded. The scan-stop/restart bracket is handled internally because
   * NimBLE's ble_gatts_mutable() returns false if any GAP procedure is
   * active when ble_gatts_add_svcs runs (silent service registration drop).
   */
  void activateGattServices(Config* cfg, Preferences* prefs);

  /**
   * @brief update the 9th manufacturer-data byte to reflect mesh state.
   * Idempotent — re-applies setManufacturerData only when the flag flips,
   * so it's safe (and cheap) to call every loop iteration. Older app
   * builds read 8-byte payloads so the layout remains backwards
   * compatible.
   * @param onMesh true iff the lamp is currently joined to the mesh
   *  (`wifi::isConnected()` is the current source of truth).
   */
  void setMeshState(bool onMesh);
};
}  // namespace lamp
#endif