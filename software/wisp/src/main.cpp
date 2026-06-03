// wisp — palette bridge + maintenance carrier.
// Phase B: Aurora ingest. ESP-NOW carries the lamp roster, and the Aurora
// palette client subscribes to a discovered Aurora device's live palette
// stream. The first zone we hear from "claims" wisp — subsequent zones are
// ignored until the spec adds zone-selection in a later phase.
//
// WiFi station-mode bring-up is intentionally NOT performed here. The user
// hasn't picked an SSID yet; Phase D wires that in via the BLE-proxied app
// pane. Without WiFi, AuroraPaletteClient will sit in Discovering and log
// mDNS failures — that's fine for build verification.

#include <Arduino.h>

#include <cstring>

#include "CurrentPalette.h"
#include "LampInventory.h"
#include "MeshLink.h"
#include "PaintDistributor.h"
#include "StatusBeacon.h"
#include "aurora/AuroraPaletteClient.h"
#include "lamp_protocol.hpp"

namespace {

wisp::MeshLink mesh;
wisp::LampInventory inventory;
wisp::CurrentPalette currentPalette;
wisp::PaintDistributor paintDistributor;
wisp::StatusBeacon statusBeacon;
AuroraPaletteClient auroraClient;

// Phase C.4 temporary serial command buffer. Phase D's BLE proxy replaces
// this with MSG_WISP_OP from the app pane.
String serialLineBuf;

// First Aurora zone we hear from latches in here. Until the wisp has a way to
// pick a zone (app pane, later phase), the first one wins; later zones log but
// don't overwrite. 0 is a real zone in Aurora, so sentinel is -1.
int firstSeenZone = -1;

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

// Aurora palette callback. Runs from auroraClient.loop() on the main task, so
// touching globals here is fine. We claim the first zone and ignore others.
void onAuroraPalette(int zone, const Palette& p) {
  if (firstSeenZone < 0) {
    firstSeenZone = zone;
    Serial.printf("[wisp] claimed Aurora zone %d\n", zone);
  } else if (zone != firstSeenZone) {
    Serial.printf("[wisp] ignoring zone %d palette (claimed %d)\n",
                  zone, firstSeenZone);
    return;
  }

  currentPalette.update(p, millis());
  const auto& cols = currentPalette.colors();
  Serial.printf("[wisp] palette change: %s with %u colors\n",
                currentPalette.paletteId().c_str(),
                (unsigned)cols.size());
  for (size_t i = 0; i < cols.size(); ++i) {
    Serial.printf("  [%u] r=%u g=%u b=%u w=%u\n",
                  (unsigned)i, cols[i].r, cols[i].g, cols[i].b, cols[i].w);
  }
  // Notify the paint distributor; it only acts if paintMode is on.
  paintDistributor.onPaletteChanged();
}

// Phase C.4 serial command handler. Parses one stripped line at a time.
// Returns nothing — anything unknown logs back, anything known logs ack.
void processSerialCommand(const String& cmd) {
  if (cmd.length() == 0) return;
  if (cmd == "paint:on") {
    paintDistributor.setPaintMode(true);
    Serial.println("[wisp.cmd] paint mode ON");
  } else if (cmd == "paint:off") {
    paintDistributor.setPaintMode(false);
    Serial.println("[wisp.cmd] paint mode OFF");
  } else {
    Serial.printf("[wisp.cmd] unknown command: %s\n", cmd.c_str());
  }
}

// Drain whatever is in the Serial RX FIFO into serialLineBuf, dispatching
// on newline. Kept inside loop() so we don't need a dedicated task — the
// FreeRTOS timer handles HELLO emission so loop() pacing here is fine.
void pumpSerial() {
  while (Serial.available() > 0) {
    int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\r') continue;  // strip CR; macOS / Linux send LF only
    if (ch == '\n') {
      String cmd = serialLineBuf;
      cmd.trim();
      serialLineBuf = String();
      processSerialCommand(cmd);
      continue;
    }
    if (serialLineBuf.length() < 64) serialLineBuf += static_cast<char>(ch);
  }
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

// Build a stable instance id from the chip MAC's low 24 bits. Aurora uses it
// to recognize a returning subscriber; we want it consistent across reboots.
String buildInstanceId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[32];
  snprintf(buf, sizeof(buf), "wisp-%06lx",
           (unsigned long)(mac & 0xFFFFFFul));
  return String(buf);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // ESP32-C6 USB-CDC takes a moment after USB enumerate to be ready for
  // printf; small delay keeps the boot banner from getting swallowed.
  delay(200);
  Serial.println("wisp: phase B boot");

  mesh.onPacket(onMeshPacket);
  if (!mesh.begin()) {
    Serial.println("[wisp] mesh init failed; will retry in 5s");
  }

  // TODO Phase D: WiFi credentials come from BLE proxy via the app pane.
  // For now we leave STA mode unconfigured; AuroraPaletteClient will fail to
  // discover and log, which is intentional during phase B build verification.
  // WiFi.mode(WIFI_STA);
  // WiFi.begin(WIFI_SSID, WIFI_PASS);

  auroraClient.setInstanceId(buildInstanceId().c_str());
  auroraClient.onActivePalette(onAuroraPalette);
  auroraClient.begin();
  Serial.printf("[wisp] aurora client started as %s\n",
                buildInstanceId().c_str());

  // Phase C.4 wiring. Paint distributor needs the inventory + mesh + palette
  // to walk peers and unicast tuples. Status beacon broadcasts MSG_WISP_HELLO
  // every 2s on a FreeRTOS timer so cadence survives Aurora loop() stalls.
  paintDistributor.begin(&inventory, &mesh, &currentPalette);
  statusBeacon.begin(&mesh, &paintDistributor, &currentPalette);
  statusBeacon.startTimer();
  Serial.println("[wisp] paint distributor + status beacon online");
  Serial.println("[wisp] cmds: paint:on / paint:off");
}

void loop() {
  static uint32_t lastDumpMs = 0;
  static uint32_t lastPruneMs = 0;
  const uint32_t now = millis();

  auroraClient.loop();
  pumpSerial();
  paintDistributor.tick(now);

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

  delay(5);
}
