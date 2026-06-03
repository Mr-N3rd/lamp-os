#include "./ble_control.hpp"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <unordered_set>

#include "../../config/config.hpp"
#include "../../behaviors/configurator.hpp"
#include "../../behaviors/fade_out.hpp"  // fadeOutRebootRequested flag
#include "../../util/color.hpp"
#include "../../lamps/standard_lamp.hpp"
#include "./bluetooth.hpp"  // for lamp::scanPausedForGattClient + BLE_GAP_SCAN_TIME_MS
#include "./crypto.hpp"
#include "./nearby_lamps.hpp"
#include "./show_receiver.hpp"
#include "./wifi.hpp"

// Defined in standard_lamp.cpp. Each BLE callback does ONLY a fixed-size
// byte copy into a pending slot — zero heap allocation on Core 0. The loop
// task on Core 1 drains the slots and does all heap work (JSON parse,
// vector build, gradient construction, timestamp updates). This restores
// the single-core memory pattern the lamp used to have when the only
// transport was AsyncTCP+WebSocket (everything ran on Core 1).
void postPendingBrightness(int8_t level);
void postPendingShadeColorsJson(const char* data, size_t len);
void postPendingBaseColorsJson(const char* data, size_t len);
void postPendingKnockout(uint8_t pixel, uint8_t brightness);
void postPendingExpressionOpJson(const char* data, size_t len);
void postPendingWifiOpJson(const char* data, size_t len);
void postPendingTestActionJson(const char* data, size_t len);
void postPendingRemoteOpJson(const char* data, size_t len);
void postPendingSettingsBlobJson(const char* data, size_t len);
void postPendingApplyEffectiveBrightness();
static constexpr size_t MAX_PENDING_JSON = 256;
static constexpr size_t MAX_PENDING_OP_JSON = 512;

namespace ble_control {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static NimBLEServer*         s_server      = nullptr;
static NimBLEService*        s_service     = nullptr;
static NimBLECharacteristic* s_stateNotify = nullptr;
static NimBLECharacteristic* s_wifiStateChar = nullptr;
static NimBLECharacteristic* s_lampSectionChar  = nullptr;
static NimBLECharacteristic* s_baseSectionChar  = nullptr;
static NimBLECharacteristic* s_shadeSectionChar = nullptr;
static NimBLECharacteristic* s_exprSectionChar  = nullptr;
static NimBLECharacteristic* s_homeSectionChar  = nullptr;
static NimBLECharacteristic* s_nearbyLampsChar  = nullptr;
static lamp::Config*         s_config      = nullptr;
static Preferences*          s_prefs       = nullptr;
static bool                  s_running     = false;

// Cross-core flags driven from BLE callbacks (Core 0) and read from the
// loop task (Core 1) via the public accessors below. volatile because
// the compiler can otherwise cache the read in a register on Core 1 and
// miss the flip.
//   s_clientConnected   — a BT client (the app) is currently connected.
//                         Used by wifi::tick to skip background scans
//                         during BT sessions, and by the effective-home
//                         -mode gate to swap to "configurator" semantics.
//   s_homeModePageActive — the app has signalled (via CHAR_HOME_MODE_FOCUS)
//                         that the user is on the Home Mode setup page.
//                         Forces effectiveHomeMode TRUE while BT is up,
//                         and routes incoming CHAR_BRIGHTNESS writes to
//                         home.brightness instead of lamp.brightness.
static volatile bool         s_clientConnected   = false;
static volatile bool         s_homeModePageActive = false;

bool isClientConnected()   { return s_clientConnected;   }
bool isHomeModePageActive() { return s_homeModePageActive; }

// Per-connection auth state.  Key = connection handle, value = authed.
static std::map<uint16_t, bool> s_connAuth;
// Per-connection crypto state (nonce replay window). Lazy-inserted on first use.
static std::map<uint16_t, lamp::crypto::PerConnState> s_connCrypto;

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
// UUID → little-endian 16-byte salt (matches Dart uuidSaltLE16)
// ---------------------------------------------------------------------------

// Parse a UUID string like "5f64f4d1-d6d9-4a44-9b3f-3a8d6f7e6b40" into 16
// bytes in **reversed hex order** so it matches the Dart side's
// `uuidSaltLE16(...)`. The HKDF salt is opaque — both sides just need to
// agree on the bytes.
static std::array<uint8_t, 16> uuidSaltLE(const char* uuid) {
  std::array<uint8_t, 16> out{};
  uint8_t bytes[16];
  size_t bi = 0;
  for (size_t i = 0; uuid[i] && bi < 16; ++i) {
    if (uuid[i] == '-') continue;
    auto hex2 = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + c - 'a';
      if (c >= 'A' && c <= 'F') return 10 + c - 'A';
      return 0;
    };
    int hi = hex2(uuid[i]);
    int lo = hex2(uuid[i + 1]);
    bytes[bi++] = static_cast<uint8_t>((hi << 4) | lo);
    ++i;  // consumed two hex chars
  }
  // Reverse to LE.
  for (size_t k = 0; k < 16; ++k) out[k] = bytes[15 - k];
  return out;
}

// ---------------------------------------------------------------------------
// Incoming write dispatcher — magic-byte routing
// ---------------------------------------------------------------------------

// Decode an inbound write payload. Dispatches on the magic-byte prefix:
//   - 0x02 → AES-GCM ciphertext via lamp::crypto::decryptOp.
//     Successful decrypt implicitly authenticates the connection
//     (GCM auth-tag has already verified the lamp password).
//   - 0x01 → explicit plaintext prefix; strip it and pass through.
//   - anything else (legacy unprefixed JSON, including the webapp's bare
//     '{' first byte) → treat the whole payload as plaintext JSON.
// Plaintext writes still require a prior CHAR_AUTH success to be authed.
// Returns true on success (json populated); false to silently reject.
static bool decodeIncomingOp(const std::string& raw,
                             uint16_t handle,
                             const uint8_t* charUuidLE16,
                             const char* charShortName,
                             std::string& outJson,
                             bool& authed) {
  const auto* p = reinterpret_cast<const uint8_t*>(raw.data());
  const size_t n = raw.size();
  if (n == 0) return false;

  if (lamp::crypto::magicByte(p, n) == lamp::crypto::MAGIC_CIPHERTEXT) {
    auto& conn = s_connCrypto[handle];
    if (!lamp::crypto::decryptOp(p, n, charUuidLE16, charShortName,
                                 s_config->lamp.password, conn, outJson)) {
      return false;
    }
    s_connAuth[handle] = true;  // GCM tag IS auth
    authed = true;
    return true;
  }

  // Plaintext path. `0x01` prefix is allowed and stripped; otherwise pass through.
  size_t start = (lamp::crypto::magicByte(p, n) == lamp::crypto::MAGIC_PLAINTEXT) ? 1 : 0;
  outJson.assign(reinterpret_cast<const char*>(p + start), n - start);
  authed = isAuthed(handle);
  return true;
}

// ---------------------------------------------------------------------------
// Server callbacks — track connections for auth state and negotiate MTU
// ---------------------------------------------------------------------------

class ControlServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    uint16_t handle = connInfo.getConnHandle();
    s_connAuth[handle] = false;

    server->setDataLen(handle, 251);
    NimBLEDevice::setMTU(TARGET_MTU);

    // Request a tighter connection interval for live-preview throughput.
    // Android may decline (it weighs power saving), but when accepted
    // this widens the link from ~20 writes/sec at the default ~49ms
    // interval to ~33-66 writes/sec at 15-30ms — eliminating the queue
    // backpressure that made continuous slider drags lag.
    //   minInterval = 12 (15.0 ms), maxInterval = 24 (30.0 ms),
    //   latency = 0, supervision timeout = 400 (4.0 s).
    server->updateConnParams(handle, 12, 24, 0, 400);

    lamp::scanPausedForGattClient = true;
    NimBLEDevice::getScan()->stop();

    // Track BT-session state. The lamp no longer associates to WiFi
    // (presence-only home mode), so there's no STA to pause. wifi::tick
    // skips its background scans while this flag is set so the radio
    // can stay focused on BT.
    s_clientConnected = true;
    s_homeModePageActive = false;
    // Recompute effective home mode now that the BT session is up — the
    // gate switches from presence-based to focus-based.
    postPendingApplyEffectiveBrightness();

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] Client connected, handle=%u (BT session active)\n", handle);
#endif
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    uint16_t handle = connInfo.getConnHandle();
    s_connAuth.erase(handle);
    s_connCrypto.erase(handle);

    // Resume the central scan now that the phone is gone.
    lamp::scanPausedForGattClient = false;
    NimBLEDevice::getScan()->start(BLE_GAP_SCAN_TIME_MS);

    s_clientConnected = false;
    s_homeModePageActive = false;
    // Recompute effective home mode — gate switches back to presence-based.
    postPendingApplyEffectiveBrightness();

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
    static const auto uuid = uuidSaltLE(CHAR_AUTH);
    const uint16_t handle = connInfo.getConnHandle();
    const std::string raw = c->getValue();
    if (raw.size() > 256) return;  // password length sanity
    std::string body;
    bool authed = false;
    if (!decodeIncomingOp(raw, handle, uuid.data(), "auth", body, authed)) return;
    if (authed) {
      // Ciphertext path already set s_connAuth — nothing more to do.
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] Auth via ciphertext handle=%u OK\n", handle);
#endif
      return;
    }
    // Plaintext path: compare the decoded body against the lamp password.
    const bool accepted = (body == s_config->lamp.password);
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
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty()) return;
    uint8_t level = static_cast<uint8_t>(val[0]);
    if (level > 100) level = 100;
#ifdef LAMP_DEBUG
    // Pair with [drain] brightness t_us=... in standard_lamp.cpp to
    // measure BLE→loop latency under continuous slider drag.
    Serial.printf("[ble] brightness recv level=%u t_us=%lu\n",
                  level, (unsigned long)micros());
#endif
    // Zero-alloc on Core 0. The drain in standard_lamp.cpp::loop() reads
    // pendingBrightness on Core 1, updates config + timestamps + strip
    // brightness all on Core 1.
    postPendingBrightness(static_cast<int8_t>(level & 0x7F));
  }
};

// ---------------------------------------------------------------------------
// Shade colors — write-without-response, JSON array of hex strings
// ---------------------------------------------------------------------------

class ShadeColorsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE shadeColors len=%u\n", (unsigned)val.size());
#endif
    // Pure memcpy. The loop drain parses the JSON on Core 1.
    postPendingShadeColorsJson(val.data(), val.size());
  }
};

// ---------------------------------------------------------------------------
// Base colors — write-without-response, JSON array of hex strings
// ---------------------------------------------------------------------------

class BaseColorsCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE baseColors len=%u\n", (unsigned)val.size());
#endif
    postPendingBaseColorsJson(val.data(), val.size());
  }
};

// ---------------------------------------------------------------------------
// Base knockout — write-without-response, 2 bytes: [pixelIndex u8, brightness% u8]
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Home Mode focus — write-without-response, single u8 bool. The app
// writes 1 while the user is on the Home Mode setup page and 0 when
// they leave. Forces effectiveHomeMode TRUE while BT is up (so the user
// can preview), and routes incoming CHAR_BRIGHTNESS writes to
// homeMode.brightness. Cleared automatically on BT disconnect (in
// ControlServerCallbacks::onDisconnect).
// ---------------------------------------------------------------------------

class HomeModeFocusCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty()) return;
    const bool active = val[0] != 0;
    s_homeModePageActive = active;
    postPendingApplyEffectiveBrightness();
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] HOME_MODE_FOCUS %s\n", active ? "on" : "off");
#endif
  }
};

class BaseKnockoutCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.size() < 2) return;
    uint8_t pixelIndex = static_cast<uint8_t>(val[0]);
    uint8_t brightness = static_cast<uint8_t>(val[1]);
    if (brightness > 100) brightness = 100;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE knockout pixel=%u brightness=%u\n", pixelIndex, brightness);
#endif
    postPendingKnockout(pixelIndex, brightness);
  }
};

class ExpressionOpCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.empty() || val.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE expressionOp len=%u\n", (unsigned)val.size());
#endif
    postPendingExpressionOpJson(val.data(), val.size());
  }
};

class WifiOpCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    static const auto uuid = uuidSaltLE(CHAR_WIFI_OP);
    const uint16_t handle = connInfo.getConnHandle();
    const std::string raw = c->getValue();
    if (raw.size() > MAX_PENDING_OP_JSON + 64) return;  // headroom for prefix+tag
    std::string json;
    bool authed = false;
    if (!decodeIncomingOp(raw, handle, uuid.data(), "wifiOp", json, authed)) return;
    if (!authed) return;
    if (json.empty() || json.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE wifiOp len=%u (decoded)\n", (unsigned)json.size());
#endif
    postPendingWifiOpJson(json.data(), json.size());
  }
};

// ── Remote-op: forward a BLE control write to a far lamp via ESP-NOW ─────
class RemoteOpCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    static const auto uuid = uuidSaltLE(CHAR_REMOTE_OP);
    const uint16_t handle = connInfo.getConnHandle();
    const std::string raw = c->getValue();
    if (raw.size() > MAX_PENDING_OP_JSON + 64) return;  // headroom for prefix+tag
    std::string json;
    bool authed = false;
    if (!decodeIncomingOp(raw, handle, uuid.data(), "remoteOp", json, authed)) return;
    if (!authed) return;
    if (json.empty() || json.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE remoteOp len=%u (decoded)\n", (unsigned)json.size());
#endif
    postPendingRemoteOpJson(json.data(), json.size());
  }
};

// ── Nearby lamps: read+notify the unified per-transport list ────────────
// Pulls from nearbyLamps.getAll() and tags each entry with viaBle /
// viaEspNow flags reflecting whether the transport has carried at least
// one sighting in this entry's current lifetime. The list itself prunes
// after LAMP_PRUNE_TIME_MS, so stale entries disappear rather than going
// badge-less — using a separate per-flag recency window would make rows
// appear in the list with no badges between the recency cutoff and the
// prune cutoff (the user-visible "ghost row" bug).
//
// Cap JSON to MTU-3 (~509 B) — same pattern as buildWifiStateJson.
static std::string buildNearbyLampsJson() {
  auto lamps = lamp::nearbyLamps.getAll();
  // Sort by name for stable rendering.
  std::sort(lamps.begin(), lamps.end(),
            [](const lamp::NearbyLamp& a, const lamp::NearbyLamp& b) {
              return a.name < b.name;
            });
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  constexpr size_t SOFT_BUDGET = 480;
  for (const auto& p : lamps) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = p.name;
    // Use the freshest transport's timestamp as a single "lastSeen" for the
    // app's display sort / age cue.
    o["lastSeenMs"] = (p.lastSeenViaEspNowMs > p.lastSeenViaBleMs)
                          ? p.lastSeenViaEspNowMs
                          : p.lastSeenViaBleMs;
    o["viaBle"]    = (p.lastSeenViaBleMs    != 0);
    o["viaEspNow"] = (p.lastSeenViaEspNowMs != 0);
    if (p.hasMac) {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
      o["mac"] = macStr;
    }
    JsonArray sh = o["shade"].to<JsonArray>();
    sh.add(p.shadeColor.r); sh.add(p.shadeColor.g); sh.add(p.shadeColor.b); sh.add(p.shadeColor.w);
    JsonArray ba = o["base"].to<JsonArray>();
    ba.add(p.baseColor.r);  ba.add(p.baseColor.g);  ba.add(p.baseColor.b);  ba.add(p.baseColor.w);
    if (measureJson(doc) > SOFT_BUDGET) {
      arr.remove(arr.size() - 1);
      break;
    }
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

class NearbyLampsCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(buildNearbyLampsJson());
  }
};

// Social dispositions — read returns the full per-peer map; write replaces
// it. Auth-gated on both directions: even though the disposition values
// aren't sensitive (they're not credentials), exposing the friendship map
// to an unauthenticated scanner would leak the lamp's peer relationships.
class SocialDispositionsCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) {
      c->setValue("");
      return;
    }
    c->setValue(s_config->asDispositionsJson().c_str());
  }
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE socialDispositions len=%u\n",
                  (unsigned)val.size());
#endif
    s_config->setDispositionsFromJson(val.data(), val.size());
  }
};

void notifyNearbyLamps() {
  if (!s_nearbyLampsChar) return;
  auto json = buildNearbyLampsJson();
  s_nearbyLampsChar->setValue(json);
  s_nearbyLampsChar->notify();
}

static const char* wifiStateName(wifi::State s) {
  switch (s) {
    case wifi::IDLE:       return "idle";
    case wifi::SCANNING:   return "scanning";
    case wifi::FAILED:     return "failed";
  }
  return "unknown";
}

static std::string buildWifiStateJson(bool includeScanResults) {
  JsonDocument doc;
  doc["state"] = wifiStateName(wifi::state());
  // (No more "ssid" / "ip" — the lamp never associates in presence-only
  // mode. App reads home.ssid from CHAR_HOME_SECTION; "is the lamp
  // currently in home mode" is implicit in the BT-session UX.)
  if (!wifi::lastError().empty()) {
    doc["lastError"] = wifi::lastError();
  }
  if (includeScanResults) {
    auto results = wifi::consumeScanResults();
    if (!results.empty()) {
      // Sort strongest first so the trimmed list keeps the most useful entries.
      std::sort(results.begin(), results.end(),
                [](const wifi::ScanResult& a, const wifi::ScanResult& b) {
                  return a.rssi > b.rssi;
                });
      // Drop duplicate SSIDs (multiple BSSIDs per network are common).
      std::unordered_set<std::string> seen;
      JsonArray arr = doc["scanResults"].to<JsonArray>();
      // BLE notify payload is capped at MTU-3 (~509 B). Stop adding entries
      // once the serialized doc would exceed a soft budget so the notify
      // doesn't get rejected by NimBLE with `val > max`.
      constexpr size_t SOFT_BUDGET = 480;
      for (const auto& r : results) {
        if (r.ssid.empty()) continue;
        if (!seen.insert(r.ssid).second) continue;
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = r.ssid;
        o["rssi"] = r.rssi;
        o["encrypted"] = r.encrypted;
        if (measureJson(doc) > SOFT_BUDGET) {
          arr.remove(arr.size() - 1);
          break;
        }
      }
    }
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

class WifiStateCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    auto json = buildWifiStateJson(true);
    c->setValue(json);
  }
};

void notifyWifiState() {
  if (!s_wifiStateChar) return;
  auto json = buildWifiStateJson(true);
  s_wifiStateChar->setValue(json);
  s_wifiStateChar->notify();
}

class ExpressionTestCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    if (!isAuthed(connInfo.getConnHandle())) return;
    std::string val = c->getValue();
    if (val.size() > MAX_PENDING_OP_JSON) return;
#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE expressionTest len=%u\n", (unsigned)val.size());
#endif
    // Empty payload is the "test complete" signal — must reach the drain.
    postPendingTestActionJson(val.data(), val.size());
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
    // The full-config blob includes `lamp.password` when a password is set.
    // isAuthed() returns true on bootstrap (no password) so first-setup is
    // unaffected; once a password exists, only GCM-authed conns get data.
    if (!isAuthed(connInfo.getConnHandle())) {
      c->setValue("");
      return;
    }
    JsonDocument doc = s_config->asJsonDocument();
    String json;
    serializeJson(doc, json);
    c->setValue(json.c_str());
  }

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    static const auto uuid = uuidSaltLE(CHAR_SETTINGS_BLOB);
    const uint16_t handle = connInfo.getConnHandle();
    const std::string raw = c->getValue();
    // Settings blob can be larger than the op bound; add 64 for ciphertext overhead.
    // NimBLE already enforces MTU on the link; this is a sanity ceiling only.
    if (raw.size() > 4096 + 64) return;
    std::string json;
    bool authed = false;
    if (!decodeIncomingOp(raw, handle, uuid.data(), "settingsBlob", json, authed)) {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] settings_blob write: decrypt/decode failed\n");
#endif
      return;
    }
    if (!authed) {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] settings_blob write: not authed\n");
#endif
      return;
    }
    if (json.empty()) return;

#ifdef LAMP_DEBUG
    Serial.printf("[ble_control] WRITE settingsBlob len=%u (decoded)\n", (unsigned)json.size());
#endif

    // Hand off to Core 1's loop drain — runs AFTER expressionOp drain so
    // any just-arrived expression edits are mirrored into
    // config.expressions before settings_blob serializes + persists.
    // See standard_lamp.cpp's settings_blob drain block.
    if (json.size() > MAX_PENDING_OP_JSON) {
#ifdef LAMP_DEBUG
      Serial.printf("[ble_control] settings_blob too large for pending slot: %u > %u\n",
                    (unsigned)json.size(), (unsigned)MAX_PENDING_OP_JSON);
#endif
      return;
    }
    postPendingSettingsBlobJson(json.data(), json.size());
  }
};

// ---------------------------------------------------------------------------
// Per-section settings reads — split the old settings_blob into 5 smaller
// characteristics so each stays well under MTU and can grow independently.
// ---------------------------------------------------------------------------

class LampSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    // asLampJson() embeds `lamp.password` when one is set — same auth gate
    // as SettingsBlobCallback. Other section reads (base/shade/expr/home)
    // carry no secrets and stay open.
    if (!isAuthed(connInfo.getConnHandle())) {
      c->setValue("");
      return;
    }
    c->setValue(s_config->asLampJson().c_str());
  }
};

class BaseSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asBaseJson().c_str());
  }
};

class ShadeSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asShadeJson().c_str());
  }
};

class ExprSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asExpressionsJson().c_str());
  }
};

class HomeSectionCallback : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    c->setValue(s_config->asHomeModeJson().c_str());
  }
};

static void notifySection(NimBLECharacteristic* c, const String& json) {
  if (!c) return;
  c->setValue(json.c_str());
  c->notify();
}

void notifyLampSection()        { notifySection(s_lampSectionChar,  s_config->asLampJson());        }
void notifyBaseSection()        { notifySection(s_baseSectionChar,  s_config->asBaseJson());        }
void notifyShadeSection()       { notifySection(s_shadeSectionChar, s_config->asShadeJson());       }
void notifyExpressionsSection() { notifySection(s_exprSectionChar,  s_config->asExpressionsJson()); }
void notifyHomeModeSection()    { notifySection(s_homeSectionChar,  s_config->asHomeModeJson());    }

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

  // NimBLE 2.x default is FALSE — advertising does NOT auto-restart after a
  // client disconnects (per CHANGELOG). Without this, the first phone disconnect
  // makes the lamp permanently undiscoverable until reboot.
  s_server->advertiseOnDisconnect(true);

  s_service = s_server->createService(SERVICE_UUID);

  // Auth — write-with-response so the app receives a GATT ack.
  // App-layer crypto: AES-GCM (0x02 prefix) auto-authenticates via the
  // GCM tag; legacy plaintext writes still compare against lamp.password.
  // Bonding is no longer required at the link layer.
  s_service->createCharacteristic(CHAR_AUTH,
      NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new AuthCallback());

  // Live-preview characteristics — slider-rate writes. Declare BOTH
  // WRITE and WRITE_NR so the client can choose write-without-response
  // (the app does, via fbp_ble_client.write(withoutResponse: true)).
  // WRITE alone forces a GATT ACK round trip per write, which capped
  // throughput at ~5 Hz on the test phone at the ~49ms connection
  // interval — visibly laggy slider drag.
  static constexpr uint32_t LIVE_WRITE_PROPS =
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR;

  s_service->createCharacteristic(CHAR_BRIGHTNESS, LIVE_WRITE_PROPS)
      ->setCallbacks(new BrightnessCallback());
  s_service->createCharacteristic(CHAR_SHADE_COLORS, LIVE_WRITE_PROPS)
      ->setCallbacks(new ShadeColorsCallback());
  s_service->createCharacteristic(CHAR_BASE_COLORS, LIVE_WRITE_PROPS)
      ->setCallbacks(new BaseColorsCallback());
  s_service->createCharacteristic(CHAR_BASE_KNOCKOUT, LIVE_WRITE_PROPS)
      ->setCallbacks(new BaseKnockoutCallback());
  // Home-mode focus: app signals whether the user is on the Home Mode
  // setup page. See HomeModeFocusCallback above.
  s_service->createCharacteristic(CHAR_HOME_MODE_FOCUS, LIVE_WRITE_PROPS)
      ->setCallbacks(new HomeModeFocusCallback());
  s_service->createCharacteristic(CHAR_EXPRESSION_TEST, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ExpressionTestCallback());
  s_service->createCharacteristic(CHAR_EXPRESSION_OP, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ExpressionOpCallback());
  s_service->createCharacteristic(CHAR_WIFI_OP,
      NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new WifiOpCallback());
  s_wifiStateChar = s_service->createCharacteristic(
      CHAR_WIFI_STATE,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_wifiStateChar->setCallbacks(new WifiStateCallback());
  s_wifiStateChar->setValue(buildWifiStateJson(false));

  // Per-section settings characteristics — read + notify. Each onRead refreshes
  // the cached value from live config. NimBLE returns the cached value on a
  // GATT read, so we also seed the initial values here to handle the first read
  // before any onRead fires.
  //
  // The default per-characteristic ATT cap is set to 1024 via
  // `-D BLE_ATT_ATTR_MAX_LEN=1024` in platformio.ini — the persisted-knockout
  // base section can reach ~672 bytes, exceeding NimBLE's stock 512 cap.
  s_lampSectionChar = s_service->createCharacteristic(
      CHAR_LAMP_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_lampSectionChar->setCallbacks(new LampSectionCallback());
  s_lampSectionChar->setValue(s_config->asLampJson().c_str());

  s_baseSectionChar = s_service->createCharacteristic(
      CHAR_BASE_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_baseSectionChar->setCallbacks(new BaseSectionCallback());
  s_baseSectionChar->setValue(s_config->asBaseJson().c_str());

  s_shadeSectionChar = s_service->createCharacteristic(
      CHAR_SHADE_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_shadeSectionChar->setCallbacks(new ShadeSectionCallback());
  s_shadeSectionChar->setValue(s_config->asShadeJson().c_str());

  s_exprSectionChar = s_service->createCharacteristic(
      CHAR_EXPR_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_exprSectionChar->setCallbacks(new ExprSectionCallback());
  s_exprSectionChar->setValue(s_config->asExpressionsJson().c_str());

  s_homeSectionChar = s_service->createCharacteristic(
      CHAR_HOME_SECTION, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_homeSectionChar->setCallbacks(new HomeSectionCallback());
  s_homeSectionChar->setValue(s_config->asHomeModeJson().c_str());

  s_nearbyLampsChar = s_service->createCharacteristic(
      CHAR_NEARBY_LAMPS, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_nearbyLampsChar->setCallbacks(new NearbyLampsCallback());
  s_nearbyLampsChar->setValue(buildNearbyLampsJson());

  // Social dispositions — read + write, both auth-gated. Initial seed
  // shows whatever was loaded from NVS at boot.
  s_service->createCharacteristic(
      CHAR_SOCIAL_DISPOSITIONS,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new SocialDispositionsCallback());

  // App-layer crypto protects forwarded credentials; no link-layer bonding.
  s_service->createCharacteristic(CHAR_REMOTE_OP,
      NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new RemoteOpCallback());

  // Settings blob — write-only. Reads now go through the per-section
  // characteristics (CHAR_LAMP_SECTION etc.), each well under MTU. The
  // single-blob read path was dropped because the full config grew past 512
  // bytes after homeMode was added; see commit da5d4d9.
  // App-layer crypto: full config save is AES-GCM encrypted; no link-layer
  // bonding required.
  s_service->createCharacteristic(CHAR_SETTINGS_BLOB,
      NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new SettingsBlobCallback());

  // State notify — notify only; no write/read needed
  s_stateNotify = s_service->createCharacteristic(CHAR_STATE_NOTIFY,
                                                  NIMBLE_PROPERTY::NOTIFY);
  s_stateNotify->setValue("{}");

  s_service->start();

  // Don't touch advertising — BluetoothComponent::begin() already configures
  // the advertiser as connectable (BLE_GAP_CONN_MODE_UND) with the color-sync
  // manufacturer data the app scans for. The control GATT service attaches to
  // the GATT server and is discovered AFTER connection, not advertised in the
  // packet (a 128-bit service UUID would overflow the 31-byte adv limit).

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
    s_wifiStateChar = nullptr;
    s_lampSectionChar = nullptr;
    s_baseSectionChar = nullptr;
    s_shadeSectionChar = nullptr;
    s_exprSectionChar = nullptr;
    s_homeSectionChar = nullptr;
    s_nearbyLampsChar = nullptr;
  }

  s_server  = nullptr;
  s_running = false;
  s_connAuth.clear();
  s_connCrypto.clear();

#ifdef LAMP_DEBUG
  Serial.printf("[ble_control] GATT control service stopped\n");
#endif
}

bool isRunning() { return s_running; }

}  // namespace ble_control
