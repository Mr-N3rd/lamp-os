#ifndef LAMP_COMPONENTS_NETWORK_ESPNOW_LINK_H
#define LAMP_COMPONENTS_NETWORK_ESPNOW_LINK_H

#include <cstdint>
#include <cstddef>

namespace lamp {

// Callback signature for received ESP-NOW frames. Fires from the Wi-Fi task —
// DO NOT do heap work or block. Copy the bytes into a queue and process in loop().
using EspNowRecvFn = void (*)(const uint8_t* mac, const uint8_t* data, size_t len);

class EspNowLink {
 public:
  // Bring up Wi-Fi (STA mode, no AP join), pin the channel, init ESP-NOW,
  // register the broadcast peer, and route incoming frames to `recv`.
  // Returns true on success.
  bool begin(uint8_t channel, EspNowRecvFn recv);

  // Broadcast to FF:FF:FF:FF:FF:FF. Returns true if send queued.
  bool broadcast(const uint8_t* data, size_t len);

  // Populate `out` (6 bytes) with this device's Wi-Fi STA MAC. Caller must
  // ensure begin() has run.
  void getMac(uint8_t out[6]);

  // Public so the C trampoline in the .cpp can reach it without a friend.
  static EspNowRecvFn s_recv;
};

}  // namespace lamp

#endif
