// StatusBeacon — broadcasts MSG_WISP_HELLO every kHelloIntervalMs from a
// FreeRTOS software timer, NOT from the Arduino loop().
//
// Phase C.4. The audit driving this design:
//   - The Aurora client's loop() can stall for tens-to-hundreds of ms while
//     reading from the WebSocket (under bad WiFi conditions, easily 1s+).
//   - If HELLO emission piggybacked on loop(), peers would see stale wisp
//     presence and cascade roles would flap.
//   - FreeRTOS timer service runs on a dedicated task, immune to loop()
//     stalls. The timer callback is non-blocking (build frame + ESP-NOW
//     enqueue) so the timer service stays responsive.
//
// The frame is broadcast — receivers don't ACK, and a missed beacon just
// means a 2-second freshness gap. Acceptable.

#pragma once

#include <cstdint>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
using StatusBeaconTimerHandle = TimerHandle_t;
#else
// Native test build doesn't include this file — but if it ever did, give
// the symbol a placeholder so nothing trips. Tests should not instantiate
// StatusBeacon; the class touches FreeRTOS + WiFi.
using StatusBeaconTimerHandle = void*;
#endif

namespace wisp {

class CurrentPalette;
class MeshLink;
class PaintDistributor;

class StatusBeacon {
 public:
  void begin(MeshLink* mesh, PaintDistributor* paint,
             CurrentPalette* palette);

  // One-shot wiring of the FreeRTOS software timer. Call once after begin().
  void startTimer();

  // Public so the C-style timer trampoline can reach it. Not part of the
  // sanctioned API — call begin()+startTimer() from main.cpp and forget.
  void emit();

 private:
  MeshLink* mesh_ = nullptr;
  PaintDistributor* paint_ = nullptr;
  CurrentPalette* palette_ = nullptr;
  uint16_t seqCounter_ = 0;
  StatusBeaconTimerHandle timer_ = nullptr;

  static constexpr uint32_t kHelloIntervalMs = 2000;
  static constexpr uint32_t kWispVersion     = 0x00010000u;  // 1.0.0
};

}  // namespace wisp
