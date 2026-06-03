#include "MeshLink.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <cstring>

namespace wisp {

MeshLink* MeshLink::s_instance = nullptr;

// ESP-NOW max payload per spec is 250 B; reject anything outside [0, 250]
// so a driver error can't cast to a huge size_t and reach handler_ with a
// bogus length.
static constexpr int kMaxRecvFrameLen = 250;

static void recvTrampoline(const esp_now_recv_info_t* info,
                           const uint8_t* data, int len) {
  if (!MeshLink::s_instance || !info) return;
  if (len < 0 || len > kMaxRecvFrameLen) return;
  auto& handler = MeshLink::s_instance->handler_;
  if (!handler) return;
  handler(info->src_addr, data, static_cast<size_t>(len));
}

bool MeshLink::begin() {
  s_instance = this;

  // Order matters: STA mode → disconnect from any AP but KEEP RADIO ON →
  // pin channel → init ESP-NOW. ESP-NOW only works while the WiFi radio
  // is powered. Calling WiFi.disconnect(true, true) was a bug — the
  // second arg is `wifioff` and turning it on shuts the radio down,
  // which silently broke recv (no frames seen, MAC reads as 00:00:..).
  // Use disconnect(false, false) — clear the AP context, keep radio up.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.setSleep(false);

  // Xiao ESP32-C6 external antenna select. The arduino-esp32 v3.x C6
  // variant declares `static const uint8_t WIFI_ANT_CONFIG = 14;` as a
  // C++ constant, NOT a #define — so #ifdef misses it. Check the board
  // macro directly. ARDUINO_XIAO_ESP32C6 comes from the variant boards.txt.
  // Without this the wisp falls back to the internal antenna and signal
  // is too weak to reach lamps in another room.
#if defined(ARDUINO_XIAO_ESP32C6) || defined(WIFI_ANT_CONFIG)
  pinMode(WIFI_ANT_CONFIG, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG, HIGH);
#endif

  esp_wifi_set_channel(LAMP_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[mesh] esp_now_init failed");
    return false;
  }

  esp_now_register_recv_cb(recvTrampoline);

  // Broadcast peer. channel=0 means "use the radio's current channel" — we
  // just pinned it above so this lands on LAMP_ESPNOW_CHANNEL.
  esp_now_peer_info_t peer = {};
  std::memset(&peer, 0, sizeof(peer));
  std::memset(peer.peer_addr, 0xFF, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[mesh] add_peer(broadcast) failed");
    return false;
  }

  uint8_t mac[6];
  getMac(mac);
  Serial.printf("[mesh] ready ch=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                LAMP_ESPNOW_CHANNEL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

void MeshLink::onPacket(MeshRecvFn handler) {
  handler_ = std::move(handler);
}

bool MeshLink::broadcast(const uint8_t* data, size_t len) {
  static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return esp_now_send(bcast, data, len) == ESP_OK;
}

bool MeshLink::peerAlreadyAdded(const uint8_t mac[6]) const {
  for (size_t i = 0; i < peerCount_; i++) {
    if (std::memcmp(peers_[i], mac, 6) == 0) return true;
  }
  return false;
}

bool MeshLink::send(const uint8_t targetMac[6], const uint8_t* data, size_t len) {
  if (!targetMac || !data) return false;
  if (!peerAlreadyAdded(targetMac)) {
    if (peerCount_ < MAX_TRACKED_PEERS) {
      esp_now_peer_info_t peer = {};
      std::memset(&peer, 0, sizeof(peer));
      std::memcpy(peer.peer_addr, targetMac, 6);
      peer.channel = 0;
      peer.ifidx = WIFI_IF_STA;
      peer.encrypt = false;
      esp_err_t err = esp_now_add_peer(&peer);
      if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
        std::memcpy(peers_[peerCount_++], targetMac, 6);
      } else {
        return false;
      }
    }
    // If we ran out of tracked slots, just try sending — ESP-NOW will report
    // a not-found error to the send callback, which the distributor can react
    // to in a later phase.
  }
  return esp_now_send(targetMac, data, len) == ESP_OK;
}

void MeshLink::getMac(uint8_t out[6]) const {
  esp_wifi_get_mac(WIFI_IF_STA, out);
}

}  // namespace wisp
