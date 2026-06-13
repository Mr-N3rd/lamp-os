#pragma once
#include <Arduino.h>
#include <Preferences.h>

#include "config/config.hpp"

namespace ble_control {

// ── Service UUID ──────────────────────────────────────────────────────────
constexpr const char* SERVICE_UUID          = "5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40";

// ── Characteristic UUIDs ──────────────────────────────────────────────────
// auth (write-with-response): send lamp.password to unlock the connection
constexpr const char* CHAR_AUTH            = "5f64f4d1-d6d9-4a44-9b3f-3a8d6f7e6b40";
// brightness (write-without-response): single u8 value 0-100. Firmware
// routes to homeMode.brightness when the app has signalled it's on the
// Home Mode page via CHAR_HOME_MODE_FOCUS; otherwise to lamp.brightness.
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
// wifi_op (write-with-response): JSON op for WiFi management. Reduced to:
//   {"op":"scan"}    — kick off an async scan (results notified via
//                       CHAR_WIFI_STATE).
//   {"op":"forget"}  — clear config.homeMode.ssid and persist. No STA
//                       association ever happens (presence-only mode),
//                       so there's no "connect" op anymore.
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
// nearby_lamps (read + notify): JSON array of every lamp this lamp has
// heard via either transport (BLE manufacturer-data adv or ESP-NOW HELLO).
constexpr const char* CHAR_NEARBY_LAMPS    = "5f64f4e3-d6d9-4a44-9b3f-3a8d6f7e6b40";
// remote_op (write-with-response, encrypted): forward a BLE control write
// to a far lamp via ESP-NOW.
constexpr const char* CHAR_REMOTE_OP       = "5f64f4e4-d6d9-4a44-9b3f-3a8d6f7e6b40";
// home_mode_focus (write-without-response): single u8 bool. The app
// writes 1 while the user is on the Home Mode setup page and 0 when
// they leave. While 1, the firmware overrides effectiveHomeMode to
// TRUE (so the user can preview home brightness / behavior) AND
// routes incoming CHAR_BRIGHTNESS writes to config.homeMode.brightness
// instead of config.lamp.brightness. While 0 (and a BT client is
// connected), effectiveHomeMode is forced FALSE — the BT session is
// the "configurator" and shouldn't be in home mode. When no BT client
// is connected, effectiveHomeMode is presence-based: home.enabled +
// home.ssid + wifi::homeSsidVisible(). Cleared automatically on BT
// disconnect.
constexpr const char* CHAR_HOME_MODE_FOCUS = "5f64f4e5-d6d9-4a44-9b3f-3a8d6f7e6b40";
// social_dispositions (read + write, encrypted): JSON map of peer name to
// disposition 1..5 (salty .. neutral .. smitten). Read returns the full
// map; write replaces it. Auth-gated like the other write characteristics.
// Stored separately from the main config blob in NVS namespace "lamp",
// key "dispositions" — survives reboots, doesn't bloat CHAR_LAMP_SECTION.
constexpr const char* CHAR_SOCIAL_DISPOSITIONS = "5f64f4e6-d6d9-4a44-9b3f-3a8d6f7e6b40";
// wisp_op (write-with-response, plaintext JSON): forward a wisp-bound op
// from the app over ESP-NOW. The lamp does NOT apply this locally — it
// only broadcasts a MSG_CONTROL_OP carrying the payload, and the wisp(s)
// on the mesh consume it. Open-set by design: the wire format is
// {"char":"wispOp","op":"setZone","zoneId":N,...} and the wisp owns the
// op vocabulary; lamps never interpret. Auth-gated like the rest of the
// write surface.
constexpr const char* CHAR_WISP_OP         = "5f64f4e1-d6d9-4a44-9b3f-3a8d6f7e6b40";
// wisp_status (read + notify): JSON snapshot of the latest wispStatus
// broadcast the lamp has seen, merged with the last MSG_WISP_HELLO data
// on file for the same wisp. Auth-gated. Notifies whenever the cache
// updates (drain of pendingWispStatus).
constexpr const char* CHAR_WISP_STATUS     = "5f64f4e2-d6d9-4a44-9b3f-3a8d6f7e6b40";

// ── Firmware OTA (Phase 5a) ───────────────────────────────────────────────
//
// fw_control (write + notify): app pushes MSG_FW_OFFER and MSG_FW_DONE
// frames (lamp_protocol wire format, no envelope) and receives
// MSG_FW_ACCEPT, MSG_FW_REQ, and MSG_FW_RESULT back as notifications.
// Auth-gated. Single in-flight OTA per lamp (mutex enforced in
// FirmwareReceiver); a write while another source is mid-flow yields a
// DeclineBusy notification.
constexpr const char* CHAR_FW_CONTROL      = "5f64f4e7-d6d9-4a44-9b3f-3a8d6f7e6b40";
// fw_chunk (write-without-response): app streams MSG_FW_CHUNK payloads.
// Auth-gated. Frequency is high (~one chunk per BLE PDU); the write
// callback parses the frame on the BLE host task and calls
// FirmwareReceiver::handleChunkOnRecvTask directly — same fast path as
// the ESP-NOW chunk handler.
constexpr const char* CHAR_FW_CHUNK        = "5f64f4e8-d6d9-4a44-9b3f-3a8d6f7e6b40";

/**
 * @brief Start the BLE GATT control service.
 */
void start(lamp::Config* config, Preferences* prefs);

void stop();
bool isRunning();

/**
 * @brief Per-loop housekeeping on Core 1. For each section whose JSON
 *        cache (on Config) is dirty, rebuild it and push the bytes into
 *        the corresponding NimBLE characteristic via setValue() — so
 *        Core 0 BLE reads are served from NimBLE's own internal buffer
 *        without re-walking config vectors on the hot path.
 *
 *        Cheap when nothing is dirty: six bool checks. Should be called
 *        once per main-loop iteration from standard_lamp.cpp::loop().
 *
 *        Audit fix #6/#7: closes the cross-core race between BLE GATT
 *        reads on Core 0 and config-mutating loop drains on Core 1, and
 *        eliminates the per-read JSON re-serialisation cost.
 */
void tick();

/**
 * @brief Send a state-change notification to all subscribed clients.
 */
void notifyStateChange();

/**
 * @brief Send a WiFi-state-change notification on CHAR_WIFI_STATE.
 */
void notifyWifiState();

/**
 * @brief Notify subscribers that the lamp section changed.
 */
void notifyLampSection();

/**
 * @brief Notify subscribers that the nearby-lamp list changed.
 */
void notifyNearbyLamps();

/**
 * @brief Push a CHAR_WISP_STATUS notification. Called from the loop
 *        drain on Core 1 after pendingWispStatus updates the cache via
 *        NearbyLamps::cacheWispStatus. The notify payload is the same
 *        merged JSON the on-read callback serves.
 */
void notifyWispStatus();

/**
 * @brief True while a BT client is currently connected to this lamp.
 *        Used by effective-home-mode logic and by wifi::tick() to skip
 *        background scans during BT sessions.
 */
bool isClientConnected();

/**
 * @brief True while the app has signalled it's on the Home Mode setup
 *        page (via CHAR_HOME_MODE_FOCUS = 1). Cleared on BT disconnect.
 */
bool isHomeModePageActive();

/**
 * @brief True while the central scan is paused because a GATT client is
 *        connected. bluetooth.cpp's onScanEnd queries this to decide
 *        whether to auto-restart the scan. Flipped on GATT connect /
 *        disconnect inside ble_control.cpp.
 */
bool isScanPaused();

/**
 * @brief Record that a BLE-originated write just landed. Stamps an
 *        internal lastWrite timestamp that tick() compares against
 *        kBleIdleThresholdMs to decide whether to widen the conn-params
 *        interval. Cheap: a single millis() store. Call at the top of
 *        any onWrite handler that processes app-originated traffic
 *        (BrightnessCallback, BaseKnockoutCallback, HomeModeFocusCallback,
 *        SocialDispositionsCallback, SettingsBlobCallback, AuthCallback,
 *        and WriteRouter::onWrite via a forward-decl in write_router.hpp).
 *        Reads are decidedly NOT activity for this purpose — pure-read
 *        traffic doesn't generate the BLE coex pressure we're trying to
 *        get out of the way of.
 */
void markActivity();

}  // namespace ble_control

namespace lamp { class FirmwareReceiver; }

namespace ble_control {

/**
 * @brief Wire the FirmwareReceiver instance into the BLE OTA dispatch.
 *        Called from standard_lamp.cpp after firmwareReceiver.begin().
 *        Once registered, CHAR_FW_CONTROL writes (OFFER/DONE) and
 *        CHAR_FW_CHUNK writes route into the receiver, and the receiver's
 *        BLE transport notifies ACCEPT/REQ/RESULT back on CHAR_FW_CONTROL.
 */
void setFirmwareReceiver(lamp::FirmwareReceiver* receiver);

}  // namespace ble_control
