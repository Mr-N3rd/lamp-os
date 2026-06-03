#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace wisp {

// ESP-NOW channel the lamp grid sits on. Matches LAMP_ESPNOW_CHANNEL in
// software/lamp-os/src/components/network/wifi.cpp. If the lamp side ever
// moves, mirror here.
#ifndef LAMP_ESPNOW_CHANNEL
#define LAMP_ESPNOW_CHANNEL 1
#endif

// Handler signature for inbound frames. Fires from the WiFi recv task —
// no heap, no blocking, no logging. Copy bytes out and process on loop.
using MeshRecvFn = std::function<void(const uint8_t srcMac[6],
                                      const uint8_t* data, size_t len)>;

/**
 * @brief Thin ESP-NOW wrapper for wisp.
 *
 * - `begin()` claims the radio channel via `esp_wifi_set_channel`, brings up
 *   ESP-NOW, registers a single broadcast peer (FF:FF:FF:FF:FF:FF), and routes
 *   incoming frames to the registered handler.
 * - `send(...)` is a unicast helper for Phase C+ (paint, OTA chunks). Phase A
 *   only listens.
 * - `broadcast(...)` is for HELLO + paint-broadcast paths in later phases.
 *
 * Caller is responsible for putting WiFi in STA mode + disconnecting any AP
 * association BEFORE calling begin(); MeshLink only pins the channel and
 * starts ESP-NOW.
 */
class MeshLink {
 public:
  bool begin();

  // Register the inbound handler. May be called before or after begin();
  // the trampoline checks the handler is set before dispatching.
  void onPacket(MeshRecvFn handler);

  // Broadcast to FF:FF:FF:FF:FF:FF. Returns true on send queued.
  bool broadcast(const uint8_t* data, size_t len);

  // Unicast to a specific peer. Adds the peer on first use (idempotent).
  // `msgType` is informational only — the payload is already a fully formed
  // lamp_protocol frame.
  bool send(const uint8_t targetMac[6], const uint8_t* data, size_t len);

  // Wisp's STA MAC. Useful for log lines + future addressed frames.
  void getMac(uint8_t out[6]) const;

  // ESP-NOW C callback can't capture; we store the std::function here and
  // dispatch through a static trampoline that reaches in via s_instance.
  // These two are public so the trampoline in the .cpp doesn't need a
  // friend declaration — they're an internal seam, not a sanctioned API.
  static MeshLink* s_instance;
  MeshRecvFn handler_;

 private:

  // Track whether we've already added a unicast peer so add_peer doesn't
  // return DUP. The peer list is small (<= a dozen lamps); a tiny array
  // of MAC suffixes is enough.
  static constexpr size_t MAX_TRACKED_PEERS = 16;
  uint8_t peers_[MAX_TRACKED_PEERS][6];
  size_t peerCount_ = 0;
  bool peerAlreadyAdded(const uint8_t mac[6]) const;
};

}  // namespace wisp
