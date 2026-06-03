#include "./wifi.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <portmacro.h>

#include "./ble_control.hpp"  // for ble_control::isClientConnected()

// Grid channel — all unconnected grid lamps line up here so ESP-NOW peers
// can hear each other. Since we never associate to a home AP anymore,
// the radio sits on this channel permanently (modulo brief scan windows).
#ifndef LAMP_ESPNOW_CHANNEL
#define LAMP_ESPNOW_CHANNEL 1
#endif

namespace wifi {

static State s_state = IDLE;
static std::string s_lastError;
static std::vector<ScanResult> s_scanResults;          // drained by UI notify
static std::vector<std::string> s_recentSsids;         // persistent presence cache
static uint32_t s_lastScanCompleteMs = 0;
static uint32_t s_lastBackgroundScanMs = 0;
static StateChangeCallback s_cb = nullptr;

// Guards s_scanResults + s_recentSsids + s_lastScanCompleteMs against
// concurrent access. Writer: Core 1 wifi::tick() (scan-complete drain
// + startScan clear). Reader/drainer: Core 0 BLE WifiStateCallback::onRead
// → consumeScanResults() (std::move + clear). Without this guard, a
// concurrent push_back on Core 1 while Core 0 std::move's the vector
// dereferences freed memory. Same homeSsidVisible() runs on Core 1
// (same as tick) but we wrap defensively — critical sections are short
// (no allocations beyond the std::move which steals the pointer, no
// network calls, no logging). See audit finding #7 / Stability #4.
static portMUX_TYPE s_scanMux = portMUX_INITIALIZER_UNLOCKED;

// How recent a scan must be for homeSsidVisible() to trust the cache.
// Networks come and go (router restarts, user leaves home), so we time
// out the presence cache aggressively.
static constexpr uint32_t SCAN_STALENESS_MS = 90 * 1000;

// How often we kick off a background scan when idle + no BT client.
// Scans cost ~5s of radio time each — once a minute is the sweet spot
// between responsiveness and ESP-NOW receive uptime.
static constexpr uint32_t BACKGROUND_SCAN_INTERVAL_MS = 60 * 1000;

static void setState(State next) {
  if (s_state == next) return;
  s_state = next;
#ifdef LAMP_DEBUG
  Serial.printf("[wifi] state -> %d (%s)\n", (int)next, s_lastError.c_str());
#endif
  if (s_cb) s_cb();
}

void begin() {
  // Order matters: `WiFi.disconnect(true, _)` passes wifioff=true which
  // calls WiFi.mode(WIFI_OFF) on its way out, so STA mode has to be
  // (re-)enabled AFTER any disconnect call — otherwise WiFi.scanNetworks
  // returns 0 (no STA to scan from).
  WiFi.disconnect(true, true);   // wipe any stale SDK creds from a previous boot
  WiFi.mode(WIFI_STA);            // enable STA so scanNetworks works
  esp_wifi_set_channel(LAMP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

void forget() {
  s_lastError.clear();
  // Swap-and-drop pattern: the critical section just steals the buffer
  // pointer (cheap O(1) swap), and the destructors / free() run OUTSIDE
  // the portMUX. portENTER_CRITICAL disables interrupts on this core, so
  // we never run heap operations inside it.
  std::vector<std::string> drop;
  portENTER_CRITICAL(&s_scanMux);
  drop.swap(s_recentSsids);
  portEXIT_CRITICAL(&s_scanMux);
  setState(IDLE);
}

State state() { return s_state; }
std::string lastError() { return s_lastError; }

void startScan() {
#ifdef LAMP_DEBUG
  // Logged so we can rule scans in/out of any "slow during BT" reports
  // — scans only fire when BT is disconnected, but a scan started just
  // before a reconnect can spill ~5s into the BT session.
  Serial.println("[wifi] scan started");
#endif
  // Steal the existing results buffer, free OUTSIDE the critical section.
  std::vector<ScanResult> drop;
  portENTER_CRITICAL(&s_scanMux);
  drop.swap(s_scanResults);
  portEXIT_CRITICAL(&s_scanMux);
  WiFi.scanDelete();
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
  setState(SCANNING);
}

std::vector<ScanResult> consumeScanResults() {
  // Called from Core 0 (BLE WifiStateCallback::onRead). The swap is
  // O(1) — three-pointer steal — so the critical section is tight.
  // Destructors on the moved-out empty vector are trivial.
  std::vector<ScanResult> out;
  portENTER_CRITICAL(&s_scanMux);
  out.swap(s_scanResults);
  portEXIT_CRITICAL(&s_scanMux);
  return out;
}

void setStateChangeCallback(StateChangeCallback cb) { s_cb = cb; }

bool homeSsidVisible(const std::string& ssid) {
  if (ssid.empty()) return false;
  // Called from Core 1 (reapplyHomeModeState → calculateEffectiveHomeMode).
  // The writer is also Core 1 (wifi::tick), so this is single-core — but
  // we still wrap because s_recentSsids is shared state and the cost is
  // negligible (string == is a no-alloc compare). The string compares run
  // inside the critical section; the loop is bounded (~30 SSIDs max).
  bool seen = false;
  uint32_t lastMs = 0;
  portENTER_CRITICAL(&s_scanMux);
  lastMs = s_lastScanCompleteMs;
  if (lastMs != 0) {
    for (const auto& s : s_recentSsids) {
      if (s == ssid) { seen = true; break; }
    }
  }
  portEXIT_CRITICAL(&s_scanMux);
  if (!seen) return false;
  // Staleness check is OUTSIDE the critical section — millis() is fast
  // but no point holding the lock for a wall-clock read.
  return (millis() - lastMs) <= SCAN_STALENESS_MS;
}

void ensureGridChannel() {
  // No-op now that we never associate to a home AP — the radio stays on
  // LAMP_ESPNOW_CHANNEL set during begin(). Kept callable for existing
  // call sites.
}

void tick() {
  // 1. Drain a completed scan (whether UI-triggered or background).
  if (s_state == SCANNING) {
    int16_t n = WiFi.scanComplete();
    if (n >= 0) {
      // Build into LOCAL vectors first — no allocations or string ops
      // inside the portMUX. The atomic swap at the end is O(1).
      std::vector<ScanResult> newResults;
      std::vector<std::string> newSsids;
      newResults.reserve(n);
      newSsids.reserve(n);
      for (int16_t i = 0; i < n; i++) {
        auto ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        std::string ssidStr(ssid.c_str());
        newResults.push_back({
          ssidStr,
          (int8_t)WiFi.RSSI(i),
          WiFi.encryptionType(i) != WIFI_AUTH_OPEN,
        });
        newSsids.push_back(ssidStr);
      }
      const uint32_t completeMs = millis();
      // Atomic publish: swap in the new buffers, free the old ones
      // OUTSIDE the critical section.
      std::vector<ScanResult> dropResults;
      std::vector<std::string> dropSsids;
      portENTER_CRITICAL(&s_scanMux);
      dropResults.swap(s_scanResults);
      dropSsids.swap(s_recentSsids);
      s_scanResults.swap(newResults);
      s_recentSsids.swap(newSsids);
      s_lastScanCompleteMs = completeMs;
      portEXIT_CRITICAL(&s_scanMux);
      WiFi.scanDelete();
      // Re-pin to grid channel post-scan (scanNetworks may have left the
      // radio on whichever channel it finished on).
      esp_wifi_set_channel(LAMP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
      setState(IDLE);
    } else if (n == WIFI_SCAN_FAILED) {
      s_lastError = "scan";
      setState(FAILED);
    }
  }

  // 2. Periodic background scan for home-presence detection. Only when
  //    no BT client is connected — scanning during a BT session would
  //    stress the shared radio and risk LINK_SUPERVISION_TIMEOUT drops.
  //    On boot, s_lastBackgroundScanMs == 0 so the first scan fires
  //    immediately (after BT settles).
  if (s_state == IDLE && !ble_control::isClientConnected()) {
    const uint32_t now = millis();
    if (s_lastBackgroundScanMs == 0 ||
        now - s_lastBackgroundScanMs > BACKGROUND_SCAN_INTERVAL_MS) {
      s_lastBackgroundScanMs = now;
      startScan();
    }
  }
}

}  // namespace wifi
