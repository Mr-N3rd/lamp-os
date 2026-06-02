#include "./espnow_link.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <cstring>

namespace lamp {

EspNowRecvFn EspNowLink::s_recv = nullptr;

static void recvTrampoline(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (EspNowLink::s_recv == nullptr || info == nullptr) return;
  EspNowLink::s_recv(info->src_addr, data, static_cast<size_t>(len));
}

bool EspNowLink::begin(uint8_t channel, EspNowRecvFn recv) {
  s_recv = recv;

  // WiFi STA mode is already up via wifi::begin() in standard_lamp setup;
  // do NOT call WiFi.mode/disconnect/setSleep here — that would clobber the
  // radio state the wifi module relies on for periodic presence scans.
  // Channel coordination is wifi::ensureGridChannel()'s job; we just set
  // peer.channel=0 below so the peer record tracks "whatever channel the
  // radio is on right now".

  if (esp_now_init() != ESP_OK) {
    Serial.println("[espnow] esp_now_init failed");
    return false;
  }

  esp_now_register_recv_cb(recvTrampoline);

  esp_now_peer_info_t peer = {};
  std::memset(&peer, 0, sizeof(peer));
  std::memset(peer.peer_addr, 0xFF, 6);
  // channel=0 means "current channel" — works whether the radio is on the
  // home AP's channel or the wifi module pinned it to LAMP_ESPNOW_CHANNEL.
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[espnow] esp_now_add_peer(broadcast) failed");
    return false;
  }

  (void)channel;  // accepted for API symmetry; channel set by wifi module
  return true;
}

bool EspNowLink::broadcast(const uint8_t* data, size_t len) {
  static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return esp_now_send(bcast, data, len) == ESP_OK;
}

void EspNowLink::getMac(uint8_t out[6]) {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

}  // namespace lamp
