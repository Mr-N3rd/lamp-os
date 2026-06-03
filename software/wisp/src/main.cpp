// wisp — palette bridge + maintenance carrier.
// Phase A: listen-only. Bring up ESP-NOW on the lamp grid channel, decode
// HELLOs, log a roster every 10 s. No paint, no Aurora, no OTA yet.

#include <Arduino.h>

#include <cstring>

#include "LampInventory.h"
#include "MeshLink.h"
#include "lamp_protocol.hpp"

namespace {

wisp::MeshLink mesh;
wisp::LampInventory inventory;

// HELLO recv handler. Fires on the WiFi task — keep it tight; only protocol
// parse + LampInventory write (which uses a bounded mutex take).
void onMeshPacket(const uint8_t* /*srcMac*/, const uint8_t* data, size_t len) {
  const uint8_t msgType = lamp_protocol::inspect(data, len);
  if (msgType != lamp_protocol::MSG_HELLO) return;

  lamp_protocol::ParsedHello h;
  if (!lamp_protocol::parseHello(data, len, h)) return;

  const std::string peerName =
      h.nameLen ? std::string(h.name, h.nameLen) : std::string();
  inventory.recordHello(h.sourceMac, peerName, h.base, h.shade,
                        h.firmwareVersion, millis());
}

// Decode a packed semver back into a human string for the serial dump.
String formatVersion(uint32_t v) {
  uint8_t major = (v >> 16) & 0xFF;
  uint8_t minor = (v >> 8) & 0xFF;
  uint8_t patch = v & 0xFF;
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u", major, minor, patch);
  return String(buf);
}

void dumpInventory(uint32_t nowMs) {
  auto roster = inventory.snapshot();
  Serial.printf("[wisp] roster (%u lamp%s):\n",
                (unsigned)roster.size(), roster.size() == 1 ? "" : "s");
  for (const auto& e : roster) {
    const uint32_t ageMs = nowMs - e.lastSeenMs;
    Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X  %-12s  fw=%s  age=%lums\n",
                  e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5],
                  e.name.c_str(), formatVersion(e.firmwareVersion).c_str(),
                  (unsigned long)ageMs);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // ESP32-C6 USB-CDC takes a moment after USB enumerate to be ready for
  // printf; small delay keeps the boot banner from getting swallowed.
  delay(200);
  Serial.println("wisp: phase A boot");

  mesh.onPacket(onMeshPacket);
  if (!mesh.begin()) {
    Serial.println("[wisp] mesh init failed; will retry in 5s");
  }
}

void loop() {
  static uint32_t lastDumpMs = 0;
  static uint32_t lastPruneMs = 0;
  const uint32_t now = millis();

  if (now - lastDumpMs > 10000) {
    lastDumpMs = now;
    dumpInventory(now);
  }

  // Prune at half the prune window so a dropped lamp leaves the roster
  // within roughly one extra dump cycle. Cheap; just a linear scan.
  if (now - lastPruneMs > 30000) {
    lastPruneMs = now;
    inventory.prune(now, LAMP_PRUNE_TIME_MS);
  }

  delay(50);
}
