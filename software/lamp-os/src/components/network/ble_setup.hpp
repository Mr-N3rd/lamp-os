#pragma once
#include <Arduino.h>
#include <Preferences.h>

#include "../../config/config.hpp"

namespace ble_setup {

constexpr const char* SERVICE_UUID  = "5f64f4c1-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_SSID     = "5f64f4c2-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_PASSWORD = "5f64f4c3-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_NAME     = "5f64f4c4-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_APPLY    = "5f64f4c5-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_INFO     = "5f64f4c6-d6d9-4a44-9b3f-3a8d6f7e6b40";

/**
 * @brief Start the BLE GATT setup service.
 *
 * Advertises a connectable service that allows a mobile app to write WiFi
 * credentials (homeModeSSID / lamp password) and lamp name via GATT
 * characteristics. Writing the apply characteristic persists the config
 * to NVS and reboots the device.
 *
 * Must be called after NimBLEDevice::init() has already been invoked
 * (i.e. after BluetoothComponent::begin()).
 *
 * @param config  Pointer to the lamp Config object (fields will be updated).
 * @param prefs   Pointer to the Preferences instance used for NVS storage.
 */
void start(lamp::Config* config, Preferences* prefs);

void stop();
bool isRunning();

}  // namespace ble_setup
