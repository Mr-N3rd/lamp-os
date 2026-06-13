#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdint>
#include <string>
#include <vector>

#include "util/color.hpp"

// Default prune age (milliseconds) shared between callers. NimBLE scan
// timer uses this when it runs onScanEnd; if a lamp hasn't been heard on
// EITHER transport in this window, it's dropped.
#ifndef LAMP_PRUNE_TIME_MS
#define LAMP_PRUNE_TIME_MS 120000
#endif

namespace lamp {

/**
 * @brief One nearby lamp — observed via BLE manufacturer-data scan,
 *        ESP-NOW HELLO, or both. Identity is `name` (user-set + capped
 *        12 chars). Per-transport `lastSeenVia*Ms` track when we last
 *        heard from this lamp on each channel so consumers can filter:
 *
 *        - SocialBehavior cares about lastSeenViaBleMs (short-range
 *          intimacy); it doesn't greet 200 m peers.
 *        - The grid view + remote-config flow gate on lastSeenViaEspNowMs
 *          since CHAR_REMOTE_OP forwarding only works for ESP-NOW peers.
 *        - The app's "Nearby Lamps" list shows everything.
 *
 *        `mac` is only populated once we've heard at least one HELLO —
 *        BLE adv doesn't carry a stable lamp-MAC anyway (the BT MAC isn't
 *        the WiFi STA MAC the protocol uses for addressing).
 */
struct NearbyLamp {
  std::string name;
  Color baseColor = Color();
  Color shadeColor = Color();
  uint8_t mac[6] = {0};
  bool hasMac = false;
  uint32_t lastSeenViaBleMs = 0;
  uint32_t lastSeenViaEspNowMs = 0;
  bool acknowledged = false;  // SocialBehavior's per-name greeting state
  // Packed semver (major<<16|minor<<8|patch) — extracted from MSG_HELLO.
  // Only set via the ESP-NOW path; BLE adv doesn't carry it. Zero until
  // we've heard at least one HELLO from this peer.
  uint32_t firmwareVersion = 0;
  // Most recent ESP-NOW RSSI (dBm) reported by the radio for any frame
  // from this peer. `getReachableViaBle()` returns its result sorted by
  // lastRssi descending so consumers (PersonalityEngine's closest-peer
  // tracking) can do `peers.front()` for the physically nearest lamp.
  // `getReachableViaEspNow()` does NOT sort — the cascade path in
  // ExpressionManager does its own sort after filtering self + naming.
  // -127 means "unknown" (no ESP-NOW frame seen yet, or the rx_ctrl
  // block was unavailable on the recv callback); sorts to the back.
  int8_t lastRssi = -127;
};

/**
 * @brief Wisp presence cache populated from MSG_WISP_HELLO. v1 has a single
 *        global slot — the deferred multi-wisp finding lets the firmware
 *        treat the most recent hello as authoritative; C.6+ will extend to
 *        a per-mac map for room-isolated wisps. The brightness-floor check
 *        in ShowReceiver reads from this struct to decide whether an
 *        incoming brightness-override below kBrightnessOverrideMin is
 *        allowed (yes if a recent hello from the same MAC is on file).
 */
struct WispCache {
  bool present = false;
  uint8_t mac[6] = {0};
  uint32_t lastHelloMs = 0;
  uint32_t wispVersion = 0;
  uint8_t flags = 0;
  // +1 for trailing NUL so logging the string is safe; the on-wire slot
  // is 8 bytes opaque so we don't enforce ASCII.
  char paletteIdPrefix[9] = {0};
  char carriedFwChannel[9] = {0};
  uint32_t carriedFwVersion = 0;
  // Phase D: last wispStatus JSON broadcast for this wisp (verbatim
  // payload). Served on CHAR_WISP_STATUS reads merged with the hello
  // fields above. Empty until the first wispStatus has been seen.
  std::string lastStatusJson;
  uint32_t lastStatusMs = 0;
  // Latest MSG_WISP_PALETTE the lamp has heard from this wisp. The wisp
  // emits this alongside wispStatus every 30 s + on-change so the app's
  // wisp editor can read the canonical manualPalette through any
  // connected lamp instead of relying on its own per-lampId
  // SharedPreferences cache. Served base64-encoded inside
  // getWispStatusReadJson()'s `manualPalette` field. Capacity matches
  // lamp_protocol::kMaxWispPaletteColors * 3 = 150 bytes.
  uint8_t manualPaletteRgb[150] = {0};
  uint8_t manualPaletteCount = 0;
  uint32_t lastPaletteMs = 0;
};

/**
 * @brief Single source of truth for "lamps I can hear right now."
 *        Mutated from NimBLE scan callback (Core 0) and ESP-NOW HELLO
 *        recv (WiFi task); read from the loop task. SemaphoreHandle_t
 *        mutex serialises.
 */
class NearbyLamps {
 public:
  static constexpr size_t MAX_NEARBY = 32;

  NearbyLamps();

  void addOrUpdateFromBle(const std::string& name,
                          const Color& base, const Color& shade);
  void addOrUpdateFromEspNow(const std::string& name, const uint8_t mac[6],
                             const Color& base, const Color& shade,
                             uint32_t firmwareVersion = 0,
                             int8_t rssi = -127);

  // Drop entries whose most-recent sighting (max of the two transports)
  // is older than `maxAgeMs`.
  void prune(uint32_t maxAgeMs);

  // SocialBehavior: only entries whose lastSeenViaBleMs is within maxAgeMs.
  std::vector<NearbyLamp> getReachableViaBle(uint32_t maxAgeMs);

  // Cheap "is anyone in BLE range" check — early-exits on first hit, no
  // heap allocation. For callers that only care about presence (e.g.
  // mesh-state advertisement flag), not the full snapshot.
  bool hasAnyReachableViaBle(uint32_t maxAgeMs);

  // Grid view / remote-config: only entries whose lastSeenViaEspNowMs
  // is within maxAgeMs.
  std::vector<NearbyLamp> getReachableViaEspNow(uint32_t maxAgeMs);

  // Look up a peer's firmwareVersion by MAC, returning 0 if the peer is
  // unknown OR was last heard via ESP-NOW longer than maxAgeMs ago.
  // Used by the gossip-OTA path on Core 1: SocialBehavior::control()
  // queries this when iterating nearby peers to decide whether to call
  // firmwareDistributor.considerPeerForOta. maxAgeMs gates on
  // lastSeenViaEspNowMs (not BLE), since firmwareVersion is only
  // populated by ESP-NOW HELLO and BLE-only sightings carry no version.
  uint32_t getFirmwareVersionByMac(const uint8_t mac[6], uint32_t maxAgeMs);

  // Full snapshot — used by CHAR_NEARBY_LAMPS for the app's unified list.
  std::vector<NearbyLamp> getAll();

  // Mark a lamp as acknowledged. SocialBehavior calls this once per peer
  // so a re-trigger doesn't re-greet the same lamp until it prunes.
  void acknowledge(const std::string& name);

  // Wisp presence — populated by the MSG_WISP_HELLO drain in the loop
  // task. Single global slot in v1 (per the deferred multi-wisp finding).
  void cacheWispHello(const uint8_t mac[6],
                      uint32_t wispVersion,
                      uint8_t flags,
                      const char* paletteIdPrefix,  // 8 bytes; not NUL-terminated
                      const char* carriedFwChannel, // 8 bytes; not NUL-terminated
                      uint32_t carriedFwVersion);

  // Snapshot of the wisp cache. Returns a copy so the brightness-floor
  // check can compare against `mac` and `lastHelloMs` without holding
  // any lock past the read.
  WispCache getWispCache();

  // Cache the latest wispStatus JSON broadcast for a given wisp MAC.
  // Loop-task-only writer (drain of pendingWispStatus on Core 1);
  // portMAX_DELAY take. If [mac] differs from the cached hello mac, the
  // cache mac is updated and `present` is asserted — a status broadcast
  // from a previously-unseen wisp is itself proof the wisp is on the
  // mesh, regardless of whether a hello has arrived yet.
  void cacheWispStatus(const uint8_t mac[6],
                       const char* json, size_t jsonLen);

  // Cache the latest MSG_WISP_PALETTE broadcast for a given wisp MAC.
  // Same single-slot semantics as cacheWispStatus — a different MAC
  // overwrites and clears stale per-wisp data. `rgb` is `count * 3`
  // bytes of packed R, G, B. `count` is clamped to the on-wire cap
  // (kMaxWispPaletteColors) so an oversized payload silently truncates
  // rather than overflowing the cache slot. Loop-task-only writer (drain
  // of pendingWispPalette on Core 1); portMAX_DELAY take.
  void cacheWispPalette(const uint8_t mac[6],
                        const uint8_t* rgb, uint8_t count);

  // Light-touch presence ping: assert that we received a wisp-sourced
  // paint frame from [mac]. Same single-slot semantics as cacheWispHello
  // (different mac → clear stale per-wisp data), but does NOT set
  // lastHelloMs or any hello-derived fields — we never received a hello
  // in this code path. The merge in getWispStatusReadJson() guards on
  // those fields' specific timestamps before publishing them.
  //
  // Why this exists: without it, a lamp that hears a wisp's
  // MSG_OVERRIDE_COLORS (unicast paint, no relay) but not its
  // MSG_WISP_HELLO (gossip-broadcast, ≤30s heartbeat) returns "{}" for
  // wispStatus until the next hello lands — the app shows "No wisp
  // detected" even though the lamp is being actively wisp-painted.
  // Observed on hardware as the "bytes=0 puzzle".
  //
  // Called from the loop-task drain of pendingOverrideColors on Core 1,
  // but uses bounded-take (2 ms) because the BLE on-read of
  // CHAR_WISP_STATUS runs on Core 0 and takes the same mutex — without
  // the bounded-take, a contended read could be starved by a
  // back-to-back paint-frame update. On timeout we drop the update;
  // the next paint frame retries.
  void cacheWispMacFromPaint(const uint8_t mac[6]);

  // Build and return the JSON to serve on CHAR_WISP_STATUS reads.
  // Merges the cached wispStatus payload with the last MSG_WISP_HELLO
  // data. Returns "{}" if nothing has been cached for either path.
  std::string getWispStatusReadJson();

  // Snapshot of "is this lamp currently following a wisp on each
  // surface, and what's the most recently painted wisp color." The Flutter
  // app reads these through wispStatus to render the will-o'-wisp
  // indicator widget in the control screen header and to grey out
  // expressions that opt into `disabledDuringWispOverride`. RGB+W hex
  // color string; empty when the surface has never received a wisp
  // paint. Set via setLampWispStateProvider() so the depencency on the
  // ColorOverride globals stays in standard_lamp.cpp.
  struct LampWispState {
    bool        controllingBase = false;
    bool        controllingShade = false;
    std::string baseWispColor;   // 8-char hex like "AB12FF40", or empty
    std::string shadeWispColor;  // same
  };
  using LampWispStateProvider = LampWispState (*)();
  void setLampWispStateProvider(LampWispStateProvider provider) {
    lampWispStateProvider_ = provider;
  }

 private:
  LampWispStateProvider lampWispStateProvider_ = nullptr;
  std::vector<NearbyLamp> store_;
  SemaphoreHandle_t mutex_ = nullptr;
  WispCache wispCache_;

  // Caller must hold the mutex. Returns index of entry or store_.size()
  // if not found.
  size_t findIndexLocked(const std::string& name) const;
  // Caller must hold the mutex. Evicts the entry with the oldest combined
  // last-seen if the store is at capacity.
  void evictOldestIfFullLocked();
};

extern NearbyLamps nearbyLamps;  // single global instance, defined in .cpp

}  // namespace lamp
