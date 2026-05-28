#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wifi {

enum State { IDLE, SCANNING, CONNECTING, CONNECTED, FAILED };

struct ScanResult {
  std::string ssid;
  int8_t rssi;
  bool encrypted;
};

void begin();
void connect(const std::string& ssid, const std::string& password);
void disconnect();
void forget();

bool isConnected();
State state();
std::string currentSsid();
std::string currentIp();
std::string lastError();  // "auth" | "noap" | "scan" | "timeout" | "" — app maps for UI

void startScan();
std::vector<ScanResult> consumeScanResults();  // empty if not done; drains on return

using StateChangeCallback = void (*)();
void setStateChangeCallback(StateChangeCallback cb);

void tick();

// Channel coordination for ESP-NOW grid.
// When not associated to a home AP, pin the radio to LAMP_ESPNOW_CHANNEL
// so all grid peers share a channel. When associated, the AP's channel
// wins — peers on different home APs / channels can't see each other,
// which is acceptable for v1 (events deploy with home mode off).
void ensureGridChannel();

}  // namespace wifi
