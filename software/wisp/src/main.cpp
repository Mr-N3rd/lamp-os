// wisp — palette bridge + maintenance carrier.
//
// Phase D: zone-selection. The wisp is now a MSG_CONTROL_OP receiver. Selection
// can come from three places, in priority order:
//   (1) NVS-persisted choice from a prior `setZone` op (WispConfig);
//   (2) first-seen Aurora zone latch (legacy default, kept so an unconfigured
//       wisp still does something useful out of the box);
//   (3) a runtime `setZone` op from the Flutter app pane (BLE → mesh →
//       MSG_CONTROL_OP → here), which write-throughs into NVS.
//
// The wisp also keeps an `observedZones` set so the app pane can offer the
// list of zones currently advertising on the Aurora bus, even ones whose
// palettes haven't resolved yet.

#include <Arduino.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CurrentPalette.h"
#include "LampInventory.h"
#include "MeshLink.h"
#include "PaintDistributor.h"
#include "StatusBeacon.h"
#include "WispConfig.h"
#include "WispOpDispatcher.h"
#include "aurora/AuroraPaletteClient.h"
#include "lamp_protocol.hpp"

namespace {

wisp::MeshLink mesh;
wisp::LampInventory inventory;
wisp::CurrentPalette currentPalette;
wisp::PaintDistributor paintDistributor;
wisp::StatusBeacon statusBeacon;
AuroraPaletteClient auroraClient;
wisp::WispConfig wispConfig;
wisp::WispOpDispatcher wispOpDispatcher(wispConfig);

// Per-msgType dedup ring for MSG_CONTROL_OP. The wisp now joins the
// gossip-relay mesh as a receiver of CONTROL_OP frames, so the same op will
// reach us multiple times by design (sender + 1-hop relays). The 64-slot
// portMUX-guarded ring keyed on (sourceMac, msgType, seq) drops the
// re-arrivals before they hit the dispatcher.
lamp_protocol::DedupRing controlOpDedup_;

// --- ZoneSelector --------------------------------------------------------
// Process-local zone-selection state. Only `currentZone` is persistent (and
// that lives in WispConfig); everything here is RAM.
enum class ZoneSource : uint8_t { None, FirstSeen, Nvs, AppOp };

const char* zoneSourceName(ZoneSource s) {
  switch (s) {
    case ZoneSource::None:      return "none";
    case ZoneSource::FirstSeen: return "first-seen";
    case ZoneSource::Nvs:       return "nvs";
    case ZoneSource::AppOp:     return "app-op";
  }
  return "?";
}

// 16 matches Aurora's per-notification states cap; oldest-eviction FIFO
// keeps the set bounded without leaking memory if a noisy Aurora keeps
// rotating zone ids.
constexpr size_t kMaxObservedZones = 16;

struct ZoneSelector {
  int        currentZone = -1;
  ZoneSource source = ZoneSource::None;
  std::vector<int> observedZones;  // FIFO, uniqued on insert

  void observe(int zone) {
    auto it = std::find(observedZones.begin(), observedZones.end(), zone);
    if (it != observedZones.end()) return;  // already known
    if (observedZones.size() >= kMaxObservedZones) {
      observedZones.erase(observedZones.begin());  // oldest-out FIFO
    }
    observedZones.push_back(zone);
  }

  bool latchFirstSeen(int zone) {
    if (source != ZoneSource::None || currentZone >= 0) return false;
    currentZone = zone;
    source = ZoneSource::FirstSeen;
    return true;
  }

  void setFromOp(int zone) {
    currentZone = zone;
    source = ZoneSource::AppOp;
  }

  void clearFromOp() {
    currentZone = -1;
    source = ZoneSource::None;
  }
};

ZoneSelector zoneSelector;

// Phase C.4 temporary serial command buffer. Phase D's BLE proxy replaces
// this with MSG_WISP_OP from the app pane.
String serialLineBuf;

// --- Pending wispOp slot (recv-task → loop-task hand-off) ----------------
// The MSG_CONTROL_OP recv handler fires on the WiFi recv task (Core 0). We
// can't run ArduinoJson or touch Preferences from there: Preferences writes
// stall the radio, and a long parse window will drop subsequent ESP-NOW
// frames. Mirror the lamp-os pending-slot pattern — fixed-size memcpy under
// a portMUX, drain in loop().
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
// CONTROL_OP payloads are bounded by CONTROL_MAX_PAYLOAD; using that as the
// slot size means a worst-case op fits without allocating.
uint8_t pendingWispOpBuf[lamp_protocol::CONTROL_MAX_PAYLOAD] = {0};
uint16_t pendingWispOpLen = 0;
uint8_t pendingWispOpSourceMac[6] = {0};
bool pendingWispOpValid = false;

// Recv-task safe: bounded memcpy + flag flip under portMUX. No heap, no
// logging. If a previous op is still pending (drain hasn't run yet) the new
// one wins — single-slot semantics, latest intent matters most.
void postPendingWispOp(const uint8_t srcMac[6], const uint8_t* payload,
                       uint16_t payloadLen) {
  if (payloadLen > lamp_protocol::CONTROL_MAX_PAYLOAD) return;
  portENTER_CRITICAL(&pendingMux);
  std::memcpy(pendingWispOpSourceMac, srcMac, 6);
  std::memcpy(pendingWispOpBuf, payload, payloadLen);
  pendingWispOpLen = payloadLen;
  pendingWispOpValid = true;
  portEXIT_CRITICAL(&pendingMux);
}

// Loop-task: copy out under portMUX, then dispatch on a local buffer so the
// portMUX critical section stays short. Returns true if a payload was drained.
void drainPendingWispOp() {
  uint8_t localBuf[lamp_protocol::CONTROL_MAX_PAYLOAD];
  uint16_t localLen = 0;
  uint8_t localMac[6];
  bool have = false;
  portENTER_CRITICAL(&pendingMux);
  if (pendingWispOpValid) {
    std::memcpy(localMac, pendingWispOpSourceMac, 6);
    std::memcpy(localBuf, pendingWispOpBuf, pendingWispOpLen);
    localLen = pendingWispOpLen;
    pendingWispOpValid = false;
    pendingWispOpLen = 0;
    have = true;
  }
  portEXIT_CRITICAL(&pendingMux);
  if (!have) return;

  wisp::DispatchResult res = wispOpDispatcher.dispatch(localBuf, localLen);
  switch (res) {
    case wisp::DispatchResult::AppliedZoneChange: {
      // Reconcile ZoneSelector with WispConfig. If the op set a zone, latch
      // it as AppOp-sourced; if it cleared, drop back to None and let the
      // next first-seen latch take over.
      if (wispConfig.hasSelectedZone()) {
        const int newZone = wispConfig.selectedZone();
        zoneSelector.setFromOp(newZone);
        Serial.printf("[wisp] zone set by app op to %d (source=app-op)\n",
                      newZone);
      } else {
        zoneSelector.clearFromOp();
        Serial.println("[wisp] zone cleared by app op (source=none)");
      }
      break;
    }
    case wisp::DispatchResult::AppliedWifiChange:
      // Stub for this phase; later task wires STA bring-up. The dispatcher
      // already stored credentials into WispConfig.
      Serial.println("[wisp] wifi creds updated (stub — no STA bring-up yet)");
      break;
    case wisp::DispatchResult::Ignored:
    case wisp::DispatchResult::Malformed:
      // Nothing to do; dispatcher already logged what mattered.
      break;
  }
}

// HELLO + CONTROL_OP recv handler. Fires on the WiFi task — keep it tight;
// only protocol parse + bounded memcpy. No logging, no Preferences, no
// ArduinoJson.
void onMeshPacket(const uint8_t* /*srcMac*/, const uint8_t* data, size_t len) {
  const uint8_t msgType = lamp_protocol::inspect(data, len);
  if (msgType == lamp_protocol::MSG_HELLO) {
    lamp_protocol::ParsedHello h;
    if (!lamp_protocol::parseHello(data, len, h)) return;
    const std::string peerName =
        h.nameLen ? std::string(h.name, h.nameLen) : std::string();
    inventory.recordHello(h.sourceMac, peerName, h.base, h.shade,
                          h.firmwareVersion, millis());
    return;
  }
  if (msgType == lamp_protocol::MSG_CONTROL_OP) {
    lamp_protocol::ParsedControlOp op;
    if (!lamp_protocol::parseControlOp(data, len, op)) return;
    // Dedup BEFORE post: a gossip-relayed duplicate must not displace a
    // pending fresh op. Per-msgType ring keyed on sourceMac+seq.
    if (!controlOpDedup_.record(op.sourceMac, lamp_protocol::MSG_CONTROL_OP,
                                op.seq)) {
      return;
    }
    postPendingWispOp(op.sourceMac, op.payload, op.payloadLen);
    return;
  }
}

// Aurora palette callback. Runs from auroraClient.loop() on the main task.
void onAuroraPalette(int zone, const Palette& p) {
  // (Observed-zones is added separately via onZoneObserved_ — that fires
  //  on every state announcement, not just resolved palettes. Still safe
  //  to also record here in case a resolve outpaces the announce path.)
  zoneSelector.observe(zone);

  // First-seen latch: only when neither NVS nor an app op has chosen a zone.
  if (zoneSelector.latchFirstSeen(zone)) {
    Serial.printf("[wisp] claimed Aurora zone %d (source=first-seen)\n", zone);
  }

  if (zone != zoneSelector.currentZone) {
    Serial.printf("[wisp] ignoring zone %d palette (selected %d, source=%s)\n",
                  zone, zoneSelector.currentZone,
                  zoneSourceName(zoneSelector.source));
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

void dumpInventory(uint32_t /*nowMs*/) {
  // Re-sample millis() here rather than trusting the loop()-top nowMs.
  // HELLOs can arrive between the loop-top sample and this dump call
  // (auroraClient.loop() can run for hundreds of ms between them), and
  // lastSeenMs gets stamped with the recv-time millis(). If lastSeenMs
  // is fresher than the captured nowMs, the unsigned subtraction wraps
  // and we'd print age=4294965031ms instead of 18ms.
  const uint32_t nowMs = millis();
  auto roster = inventory.snapshot();
  Serial.printf("[wisp] roster (%u lamp%s):\n",
                (unsigned)roster.size(), roster.size() == 1 ? "" : "s");
  for (const auto& e : roster) {
    // Defensive: if lastSeenMs is still somehow ahead of now (e.g. millis()
    // overflow boundary), clamp to 0 instead of wrapping.
    const uint32_t ageMs = (nowMs >= e.lastSeenMs) ? nowMs - e.lastSeenMs : 0;
    Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X  %-12s  fw=%s  age=%lums\n",
                  e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5],
                  e.name.c_str(), formatVersion(e.firmwareVersion).c_str(),
                  (unsigned long)ageMs);
  }
  // Phase D: log the ZoneSelector state alongside the roster so the dump
  // is one-stop for "what's this wisp doing?".
  Serial.printf("[wisp] zone=%d source=%s observed=%u\n",
                zoneSelector.currentZone,
                zoneSourceName(zoneSelector.source),
                (unsigned)zoneSelector.observedZones.size());
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
  Serial.println("wisp: phase D boot");

  // Bring NVS up before anything that might want to read selZone. The
  // ZoneSelector seeds itself from the cached value here.
  wispConfig.begin();
  if (wispConfig.hasSelectedZone()) {
    zoneSelector.currentZone = wispConfig.selectedZone();
    zoneSelector.source = ZoneSource::Nvs;
    Serial.printf("[wisp] zone %d from NVS\n", zoneSelector.currentZone);
  } else {
    Serial.println("[wisp] no zone in NVS; will latch first-seen Aurora zone");
  }

  mesh.onPacket(onMeshPacket);
  if (!mesh.begin()) {
    Serial.println("[wisp] mesh init failed; will retry in 5s");
  }

  // TODO Phase D follow-up: WiFi STA bring-up reads from wispConfig.wifiSsid()
  // once the setWifi op is no longer a stub. For now we leave STA mode
  // unconfigured; AuroraPaletteClient will fail to discover and log.
  // WiFi.mode(WIFI_STA);
  // WiFi.begin(WIFI_SSID, WIFI_PASS);

  auroraClient.setInstanceId(buildInstanceId().c_str());
  auroraClient.onActivePalette(onAuroraPalette);
  // Phase D: capture every zone we hear about, not just ones whose palettes
  // resolve. This fires on the main task from inside auroraClient.loop().
  auroraClient.onZoneObserved([](int zone) { zoneSelector.observe(zone); });
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
  // Drain any pending MSG_CONTROL_OP payload posted by the recv task. Cheap
  // when empty (one portMUX read + bool check), so safe to call every loop.
  drainPendingWispOp();
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
