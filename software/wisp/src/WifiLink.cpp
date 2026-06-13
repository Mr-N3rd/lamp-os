#include "WifiLink.h"

#include <WiFi.h>

#include "WispConfig.h"

namespace wisp {

void WifiLink::begin(WispConfig* config) {
  if (started_) return;
  started_ = true;
  config_ = config;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t /*info*/) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("[wifi] got IP: %s\n",
                    WiFi.localIP().toString().c_str());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.println("[wifi] disconnected");
    }
  });

  // Kick the initial association from whatever WispConfig has cached.
  // If no creds are saved yet (fresh wisp) the helper logs and returns.
  reconnect();
}

void WifiLink::reconnect() {
  if (!config_) {
    Serial.println("[wifi] reconnect called before begin(); ignoring");
    return;
  }
  const std::string s = ssid();
  const std::string p = password();
  if (s.empty() || p.empty()) {
    Serial.println("[wifi] no creds; staying disconnected");
    // Drop any in-flight association so a creds-clear actually disconnects.
    // (Idempotent if we weren't associated.)
    WiFi.disconnect(true);
    return;
  }
  Serial.printf("[wifi] reconnect requested ssid=%s\n", s.c_str());
  // disconnect(true, false) drops the association without erasing the
  // stored creds inside the WiFi driver. We follow with a begin() to
  // re-associate using whatever WispConfig now holds.
  WiFi.disconnect(true, false);
  WiFi.begin(s.c_str(), p.c_str());
}

bool WifiLink::isConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

std::string WifiLink::ssid() const {
  if (!config_) return {};
  return std::string(config_->wifiSsid().c_str());
}

std::string WifiLink::password() const {
  if (!config_) return {};
  return std::string(config_->wifiPw().c_str());
}

}  // namespace wisp
