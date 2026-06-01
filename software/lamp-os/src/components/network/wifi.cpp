#include "./wifi.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Default channel used when not associated to a home AP. All grid lamps
// must agree on this for ESP-NOW broadcasts to be heard. Channel 1 is the
// standard pick for hobbyist projects (least overlap with most APs).
#ifndef LAMP_ESPNOW_CHANNEL
#define LAMP_ESPNOW_CHANNEL 1
#endif

namespace wifi {

static State s_state = IDLE;
static std::string s_ssid;
static std::string s_password;
static std::string s_lastError;
static std::vector<ScanResult> s_scanResults;
static uint32_t s_connectStartMs = 0;
static StateChangeCallback s_cb = nullptr;

static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

static void setState(State next) {
  if (s_state == next) return;
  State prev = s_state;
  s_state = next;
#ifdef LAMP_DEBUG
  Serial.printf("[wifi] state -> %d (%s)\n", (int)next, s_lastError.c_str());
#endif
  // If we just left CONNECTED (AP gone / dropped), re-pin the radio to the
  // grid channel so ESP-NOW stays usable across peers.
  if (prev == CONNECTED && next != CONNECTED) {
    esp_wifi_set_channel(LAMP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  }
  if (s_cb) s_cb();
}

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // Loosen the STA's minimum auth threshold from the Arduino default
  // (WPA2_PSK) to WPA_PSK so we'll happily associate with WPA2/WPA3
  // mixed-mode APs that advertise WPA3 first and only fall back to WPA2
  // on negotiation. Symptom of the default being too strict: ESP32
  // reports WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY (210) against
  // an AP the user can clearly see and connect to from other devices.
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
  WiFi.disconnect(true, true);  // clear any stale config left over from a previous boot
}

void connect(const std::string& ssid, const std::string& password) {
  if (ssid.empty()) { disconnect(); return; }
  s_ssid = ssid;
  s_password = password;
  s_lastError.clear();
  WiFi.begin(s_ssid.c_str(), s_password.c_str());
  s_connectStartMs = millis();
  setState(CONNECTING);
}

void disconnect() {
  WiFi.disconnect(false, false);
  if (s_state == CONNECTED || s_state == CONNECTING) setState(IDLE);
}

void forget() {
  s_ssid.clear();
  s_password.clear();
  s_lastError.clear();
  WiFi.disconnect(true, true);  // wipe SDK creds too
  setState(IDLE);
}

bool isConnected() { return s_state == CONNECTED && WiFi.status() == WL_CONNECTED; }
State state() { return s_state; }
std::string currentSsid() { return isConnected() ? s_ssid : std::string(); }
std::string currentIp() { return isConnected() ? std::string(WiFi.localIP().toString().c_str()) : std::string(); }
std::string lastError() { return s_lastError; }

void startScan() {
  s_scanResults.clear();
  WiFi.scanDelete();
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
  setState(SCANNING);
}

std::vector<ScanResult> consumeScanResults() {
  std::vector<ScanResult> out = std::move(s_scanResults);
  s_scanResults.clear();
  return out;
}

void setStateChangeCallback(StateChangeCallback cb) { s_cb = cb; }

void ensureGridChannel() {
  // If we're associated to a home AP, leave the channel alone — that AP's
  // channel wins and ESP-NOW rides on it. If not associated, pin the radio
  // to LAMP_ESPNOW_CHANNEL so all unconnected grid lamps line up.
  if (s_state == CONNECTED && WiFi.status() == WL_CONNECTED) return;
  esp_wifi_set_channel(LAMP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

void tick() {
  if (s_state == SCANNING) {
    int16_t n = WiFi.scanComplete();
    if (n >= 0) {
      s_scanResults.reserve(n);
      for (int16_t i = 0; i < n; i++) {
        auto ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        s_scanResults.push_back({
          std::string(ssid.c_str()),
          (int8_t)WiFi.RSSI(i),
          WiFi.encryptionType(i) != WIFI_AUTH_OPEN,
        });
      }
      WiFi.scanDelete();
      // Resume the connection state we were in before the scan
      if (WiFi.status() == WL_CONNECTED) setState(CONNECTED);
      else if (!s_ssid.empty())          setState(CONNECTING);
      else                                setState(IDLE);
    } else if (n == WIFI_SCAN_FAILED) {
      s_lastError = "scan";
      setState(FAILED);
    }
  }

  if (s_state == CONNECTING) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      s_lastError.clear();
      setState(CONNECTED);
    } else if (s == WL_CONNECT_FAILED) {
      s_lastError = "auth";
      setState(FAILED);
    } else if (s == WL_NO_SSID_AVAIL) {
      s_lastError = "noap";
      setState(FAILED);
    } else if (millis() - s_connectStartMs > CONNECT_TIMEOUT_MS) {
      s_lastError = "timeout";
      setState(FAILED);
    }
  }
}

}  // namespace wifi
