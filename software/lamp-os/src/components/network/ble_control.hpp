#pragma once
#include <Arduino.h>
#include <Preferences.h>

#include "../../config/config.hpp"

namespace ble_control {

// ── Service UUID ──────────────────────────────────────────────────────────
constexpr const char* SERVICE_UUID          = "5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40";

// ── Characteristic UUIDs ──────────────────────────────────────────────────
// auth (write-with-response): send lamp.password to unlock the connection
constexpr const char* CHAR_AUTH            = "5f64f4d1-d6d9-4a44-9b3f-3a8d6f7e6b40";
// brightness (write-without-response): single u8 value 0-100
constexpr const char* CHAR_BRIGHTNESS      = "5f64f4d2-d6d9-4a44-9b3f-3a8d6f7e6b40";
// shade_colors (write-without-response): JSON array of hex color strings
constexpr const char* CHAR_SHADE_COLORS    = "5f64f4d3-d6d9-4a44-9b3f-3a8d6f7e6b40";
// base_colors (write-without-response): JSON array of hex color strings
constexpr const char* CHAR_BASE_COLORS     = "5f64f4d4-d6d9-4a44-9b3f-3a8d6f7e6b40";
// base_knockout (write-without-response): 2 bytes [pixelIndex u8, brightness u8 0-100]
constexpr const char* CHAR_BASE_KNOCKOUT   = "5f64f4d5-d6d9-4a44-9b3f-3a8d6f7e6b40";
// expression_test (write-with-response): UTF-8 expression type name; empty string = complete
constexpr const char* CHAR_EXPRESSION_TEST = "5f64f4d6-d6d9-4a44-9b3f-3a8d6f7e6b40";
// settings_blob (read + write-with-response): full config JSON
constexpr const char* CHAR_SETTINGS_BLOB   = "5f64f4d7-d6d9-4a44-9b3f-3a8d6f7e6b40";
// state_notify (notify): lamp-driven state change notifications
constexpr const char* CHAR_STATE_NOTIFY    = "5f64f4d8-d6d9-4a44-9b3f-3a8d6f7e6b40";

/**
 * @brief Start the BLE GATT control service.
 *
 * Exposes brightness, color, expression-test, settings, and auth
 * characteristics over GATT so a mobile app can control the lamp without
 * WiFi. Writes are gated by the lamp password when one is configured.
 *
 * Must be called after NimBLEDevice::init() has already been invoked
 * (i.e. after BluetoothComponent::begin()).
 *
 * @param config  Pointer to the lamp Config object.
 * @param prefs   Pointer to the Preferences instance used for NVS storage.
 */
void start(lamp::Config* config, Preferences* prefs);

void stop();
bool isRunning();

/**
 * @brief Send a state-change notification to all subscribed clients.
 *
 * Called internally after brightness changes and settings saves, but may
 * also be triggered from outside (e.g. after home-mode brightness auto-adjust).
 */
void notifyStateChange();

}  // namespace ble_control
