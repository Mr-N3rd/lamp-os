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
// expression_op (write-with-response): JSON op for runtime expression CRUD
//   {"op":"upsert","entry":{...full expression config...}}
//   {"op":"remove","type":"<type>","target":<1|2|3>}
constexpr const char* CHAR_EXPRESSION_OP   = "5f64f4d9-d6d9-4a44-9b3f-3a8d6f7e6b40";
// wifi_op (write-with-response): JSON op for WiFi STA management
//   {"op":"scan"} | {"op":"connect","ssid":"...","password":"..."} | {"op":"forget"}
constexpr const char* CHAR_WIFI_OP         = "5f64f4da-d6d9-4a44-9b3f-3a8d6f7e6b40";
// wifi_state (read + notify): JSON snapshot of WiFi state
constexpr const char* CHAR_WIFI_STATE      = "5f64f4db-d6d9-4a44-9b3f-3a8d6f7e6b40";
// Per-section settings characteristics (read + notify). Replace the single
// settings_blob read path — each stays well under MTU on its own.
constexpr const char* CHAR_LAMP_SECTION    = "5f64f4dc-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_BASE_SECTION    = "5f64f4dd-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_SHADE_SECTION   = "5f64f4de-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_EXPR_SECTION    = "5f64f4df-d6d9-4a44-9b3f-3a8d6f7e6b40";
constexpr const char* CHAR_HOME_SECTION    = "5f64f4e0-d6d9-4a44-9b3f-3a8d6f7e6b40";

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

/**
 * @brief Send a WiFi-state-change notification on CHAR_WIFI_STATE. Called by
 *        wifi::tick() whenever the WiFi state machine transitions.
 */
void notifyWifiState();

}  // namespace ble_control
