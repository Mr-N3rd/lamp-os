#include "StatusBeacon.h"

#include <Arduino.h>

#include <cstring>

#include "CurrentPalette.h"
#include "MeshLink.h"
#include "PaintDistributor.h"
#include "lamp_protocol.hpp"

namespace wisp {

void StatusBeacon::begin(MeshLink* mesh, PaintDistributor* paint,
                         CurrentPalette* palette) {
  mesh_ = mesh;
  paint_ = paint;
  palette_ = palette;
}

void StatusBeacon::startTimer() {
  if (timer_) return;
  timer_ = xTimerCreate(
      "wisp_hello",
      pdMS_TO_TICKS(kHelloIntervalMs),
      pdTRUE,  // auto-reload
      this,
      [](TimerHandle_t t) {
        auto* self = static_cast<StatusBeacon*>(pvTimerGetTimerID(t));
        if (self) self->emit();
      });
  if (timer_) {
    xTimerStart(timer_, 0);
  } else {
    Serial.println("[wisp.beacon] xTimerCreate failed");
  }
}

void StatusBeacon::emit() {
  if (!mesh_) return;

  uint8_t srcMac[6] = {0};
  mesh_->getMac(srcMac);

  // Flags: paintMode is authoritative. wifiConnected + auroraConnected are
  // currently hardcoded false — wiring real accessors is on the Phase D
  // list (BLE proxy needs them for the app pane state read).
  // TODO(C.5+): add MeshLink::isWifiConnected() and AuroraPaletteClient::
  // isStreaming(), pipe them through StatusBeacon::begin() and reflect here.
  uint8_t flags = 0;
  if (paint_ && paint_->paintMode()) {
    flags |= lamp_protocol::WISP_HELLO_FLAG_PAINT_MODE;
  }

  // paletteIdPrefix: low 8 bytes of the active palette id (or zeros).
  char paletteIdPrefix[lamp_protocol::WISP_HELLO_PALETTE_ID_PREFIX_LEN] = {0};
  size_t paletteIdPrefixLen = 0;
  if (palette_) {
    const std::string& id = palette_->paletteId();
    paletteIdPrefixLen = id.size() > sizeof(paletteIdPrefix)
                            ? sizeof(paletteIdPrefix)
                            : id.size();
    if (paletteIdPrefixLen) {
      std::memcpy(paletteIdPrefix, id.data(), paletteIdPrefixLen);
    }
  }

  // carriedFwChannel / carriedFwVersion: zeros for v1. Phase F populates
  // when the OTA distributor stages a firmware blob in flash.
  const char* carriedFwChannel = nullptr;
  uint32_t carriedFwVersion = 0;

  uint8_t buf[lamp_protocol::WISP_HELLO_FIXED_SIZE];
  size_t n = lamp_protocol::buildWispHello(
      buf, sizeof(buf), seqCounter_++,
      srcMac, kWispVersion, flags,
      paletteIdPrefix, paletteIdPrefixLen,
      carriedFwChannel, 0,
      carriedFwVersion);
  if (!n) return;
  mesh_->broadcast(buf, n);
}

}  // namespace wisp
