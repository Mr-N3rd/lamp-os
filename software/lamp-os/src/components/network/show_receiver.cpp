#include "show_receiver.hpp"

#include <Arduino.h>

#include <algorithm>
#include <cstring>

#include "../firmware/firmware_receiver.hpp"
#include "../../version.hpp"

namespace lamp {

ShowReceiver* ShowReceiver::s_instance = nullptr;

void ShowReceiver::onRecv(const uint8_t* mac, const uint8_t* data, size_t len,
                          int8_t rssi) {
  if (s_instance) s_instance->handleRecv(mac, data, len, rssi);
}

void ShowReceiver::begin(Config* cfg) {
  config_ = cfg;
  s_instance = this;
  if (!link_.begin(LAMP_ESPNOW_CHANNEL, &ShowReceiver::onRecv)) {
    Serial.println("[show] ESP-NOW init failed");
    return;
  }
  link_.getMac(myMac_);
  Serial.printf("[show] ready, mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                myMac_[0], myMac_[1], myMac_[2], myMac_[3], myMac_[4], myMac_[5]);
}

void ShowReceiver::tick() {
  uint32_t now = millis();
  if (now - lastHelloMs_ < LAMP_HELLO_INTERVAL_MS && lastHelloMs_ != 0) return;
  lastHelloMs_ = now;
  emitHello();
}

void ShowReceiver::getMyMac(uint8_t out[6]) const {
  std::memcpy(out, myMac_, 6);
}

void ShowReceiver::setControlOpHandler(ControlOpHandler h) {
  controlOpHandler_ = std::move(h);
}

bool ShowReceiver::sendExpressionTo(const std::string& peerName,
                                    const ExpressionInvocation& inv) {
  // Single-peer named unicast. The MSG_EVENT cascade path covers the
  // "fan out to every peer with RSSI-ordered stagger" case; this entry
  // point is preserved for callers that explicitly want to address one
  // lamp by name (BLE remoteOp targeted at a specific peer, etc.).
  auto peers = nearbyLamps.getReachableViaEspNow(LAMP_PRUNE_TIME_MS);
  for (const auto& p : peers) {
    if (p.name != peerName) continue;
    if (!p.hasMac) {
#ifdef LAMP_DEBUG
      Serial.printf("[show] sendExpressionTo: peer '%s' has no MAC yet\n",
                    peerName.c_str());
#endif
      continue;
    }
    std::string json;
    serializeInvocation(inv, json);
    if (json.size() > lamp_protocol::CONTROL_MAX_PAYLOAD) {
#ifdef LAMP_DEBUG
      Serial.printf("[show] sendExpressionTo payload %u > max %u, dropping\n",
                    (unsigned)json.size(),
                    (unsigned)lamp_protocol::CONTROL_MAX_PAYLOAD);
#endif
      return false;
    }
    return sendControlOp(p.mac,
                         reinterpret_cast<const uint8_t*>(json.data()),
                         json.size());
  }
#ifdef LAMP_DEBUG
  Serial.printf("[show] sendExpressionTo: peer '%s' not reachable\n",
                peerName.c_str());
#endif
  return false;
}

bool ShowReceiver::sendControlOp(const uint8_t targetMac[6],
                                 const uint8_t* payload, size_t payloadLen) {
  if (payloadLen > lamp_protocol::CONTROL_MAX_PAYLOAD) return false;
  uint8_t buf[lamp_protocol::CONTROL_MAX_SIZE];
  // sourceMac is THIS lamp — peers and the originator can dedup our own
  // re-broadcasts based on it.
  const size_t n = lamp_protocol::buildControlOp(buf, sizeof(buf), controlOpSeq_++,
                                                 targetMac, myMac_,
                                                 payload, payloadLen);
  if (!n) return false;
  // Record in our own dedup ring so the inbound re-broadcast (from a peer)
  // doesn't loop back as an "apply locally".
  controlOpDedup_.record(myMac_, lamp_protocol::MSG_CONTROL_OP, controlOpSeq_ - 1);
  return link_.broadcast(buf, n);
}

bool ShowReceiver::broadcastRaw(const uint8_t* data, size_t len) {
  return link_.broadcast(data, len);
}

uint16_t ShowReceiver::nextEventSeq() {
  return eventSeq_++;
}

// Decide whether `targetMac` addresses this lamp. Single helper so the
// per-message-type branches below stay readable. Broadcast = all-FF.
static bool addressedToUs(const uint8_t targetMac[6], const uint8_t myMac[6]) {
  static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return std::memcmp(targetMac, myMac, 6) == 0 ||
         std::memcmp(targetMac, bcast, 6) == 0;
}

void ShowReceiver::handleRecv(const uint8_t* /*srcMac*/, const uint8_t* data,
                              size_t len, int8_t rssi) {
  const uint8_t msgType = lamp_protocol::inspect(data, len);
  if (msgType == lamp_protocol::MSG_HELLO) {
    lamp_protocol::ParsedHello h;
    if (!lamp_protocol::parseHello(data, len, h)) return;
    if (!helloDedup_.record(h.sourceMac, lamp_protocol::MSG_HELLO, h.seq)) return;
    // Don't rebroadcast our OWN hellos.
    if (std::memcmp(h.sourceMac, myMac_, 6) == 0) return;
    // Record into the unified nearbyLamps store so the BLE CHAR_NEARBY_LAMPS
    // read + SocialBehavior + any other consumer all see this lamp. RSSI
    // feeds the cascade sort so peers physically closer to the originator
    // fire first when a MSG_EVENT cascade comes through.
    const std::string peerName = h.nameLen ? std::string(h.name, h.nameLen) : std::string();
    nearbyLamps.addOrUpdateFromEspNow(
        peerName,
        h.sourceMac,
        Color(h.base[0],  h.base[1],  h.base[2],  h.base[3]),
        Color(h.shade[0], h.shade[1], h.shade[2], h.shade[3]),
        h.firmwareVersion,
        rssi);
    link_.broadcast(data, len);
  } else if (msgType == lamp_protocol::MSG_CONTROL_OP) {
    lamp_protocol::ParsedControlOp op;
    if (!lamp_protocol::parseControlOp(data, len, op)) return;
    // Dedup by (sourceMac, seq) so a loop-relayed copy doesn't fire twice.
    if (!controlOpDedup_.record(op.sourceMac, lamp_protocol::MSG_CONTROL_OP, op.seq)) return;
    // Rebroadcast for grid relay — extends mesh reach beyond direct radio
    // range. The cascade no longer rides this path (MSG_EVENT broadcast
    // replaces the per-peer unicast hack), so CONTROL_OP is unconditionally
    // gossip-relayed now.
    link_.broadcast(data, len);
    // Apply locally if addressed to us or broadcast.
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const bool forUs = (std::memcmp(op.targetMac, myMac_, 6) == 0) ||
                       (std::memcmp(op.targetMac, bcast, 6) == 0);
    if (forUs && controlOpHandler_) {
#ifdef LAMP_DEBUG
      Serial.printf("[show] CONTROL_OP apply len=%u src=%02X:%02X:%02X:%02X:%02X:%02X\n",
                    (unsigned)op.payloadLen,
                    op.sourceMac[0], op.sourceMac[1], op.sourceMac[2],
                    op.sourceMac[3], op.sourceMac[4], op.sourceMac[5]);
#endif
      controlOpHandler_(op.payload, op.payloadLen, op.sourceMac);
    }
  } else if (msgType == lamp_protocol::MSG_WISP_HELLO) {
    // Wisp presence beacon. Parse → dedup → forward to NearbyLamps via a
    // pending slot (loop drain on Core 1 calls cacheWispHello). Keep
    // recv-task work to memcpy + dedup so the WiFi task can return
    // quickly to its next frame.
    lamp_protocol::ParsedWispHello h;
    if (!lamp_protocol::parseWispHello(data, len, h)) return;
    if (!wispHelloDedup_.record(h.sourceMac, lamp_protocol::MSG_WISP_HELLO, h.seq)) return;
    PendingWispHello slot;
    std::memcpy(slot.sourceMac, h.sourceMac, 6);
    slot.wispVersion = h.wispVersion;
    slot.flags = h.flags;
    std::memcpy(slot.paletteIdPrefix, h.paletteIdPrefix,
                lamp_protocol::WISP_HELLO_PALETTE_ID_PREFIX_LEN);
    std::memcpy(slot.carriedFwChannel, h.carriedFwChannel,
                lamp_protocol::WISP_HELLO_FW_CHANNEL_LEN);
    slot.carriedFwVersion = h.carriedFwVersion;
    postPendingWispHello(slot);
    // Rebroadcast so wisps in adjacent rooms (no direct line of sight to
    // this room) still propagate. Same gossip semantics as MSG_HELLO.
    link_.broadcast(data, len);
  } else if (msgType == lamp_protocol::MSG_OVERRIDE_COLORS) {
    lamp_protocol::ParsedOverrideColors p;
    if (!lamp_protocol::parseOverrideColors(data, len, p)) return;
    if (!overrideColorsDedup_.record(p.sourceMac,
                                     lamp_protocol::MSG_OVERRIDE_COLORS,
                                     p.seq)) return;
    // OVERRIDE/RESTORE messages do NOT relay (per design: unicast paint,
    // no gossip). Reach is whatever direct radio delivers — keeps wisp's
    // paint scoped to "lamps the wisp can hear" without spillover.
    if (!addressedToUs(p.targetMac, myMac_)) return;
    PendingOverrideColors slot;
    std::memcpy(slot.sourceMac, p.sourceMac, 6);
    slot.surface = p.surface;
    slot.sourceKind = p.sourceKind;
    slot.fadeDurationMs = p.fadeDurationMs;
    slot.numColors = p.numColors;
    for (uint8_t i = 0; i < p.numColors; ++i) {
      slot.colors[i] = Color(p.colors[i][0], p.colors[i][1],
                             p.colors[i][2], p.colors[i][3]);
    }
    postPendingOverrideColors(slot);
  } else if (msgType == lamp_protocol::MSG_RESTORE_COLORS) {
    lamp_protocol::ParsedRestoreColors p;
    if (!lamp_protocol::parseRestoreColors(data, len, p)) return;
    if (!restoreColorsDedup_.record(p.sourceMac,
                                    lamp_protocol::MSG_RESTORE_COLORS,
                                    p.seq)) return;
    // No relay — see OVERRIDE_COLORS branch.
    if (!addressedToUs(p.targetMac, myMac_)) return;
    PendingRestoreColors slot;
    std::memcpy(slot.sourceMac, p.sourceMac, 6);
    slot.surface = p.surface;
    slot.sourceKind = p.sourceKind;
    slot.fadeDurationMs = p.fadeDurationMs;
    postPendingRestoreColors(slot);
  } else if (msgType == lamp_protocol::MSG_OVERRIDE_BRIGHTNESS) {
    lamp_protocol::ParsedOverrideBrightness p;
    if (!lamp_protocol::parseOverrideBrightness(data, len, p)) return;
    if (!overrideBrightnessDedup_.record(p.sourceMac,
                                         lamp_protocol::MSG_OVERRIDE_BRIGHTNESS,
                                         p.seq)) return;
    // No relay — see OVERRIDE_COLORS branch.
    if (!addressedToUs(p.targetMac, myMac_)) return;
    // Anti-defeat brightness floor: a peer-swap or random source can't
    // drag brightness arbitrarily low. Wisp-paired sources bypass the
    // floor (the wisp owns a lobby "go dark" scenario) — check by
    // matching sourceMac against the cached MSG_WISP_HELLO sender within
    // the watchdog window.
    if (p.brightness < lamp_protocol::kBrightnessOverrideMin) {
      const auto wisp = nearbyLamps.getWispCache();
      const uint32_t now = millis();
      // 60s pairing window — matches the override watchdog. A wisp that
      // hasn't beaconed in this long loses the floor-bypass; defeat-by-
      // silent-source is then the same risk as for any other unknown
      // sender (i.e. dropped).
      constexpr uint32_t kWispPairingWindowMs = 60000;
      const bool wispPaired = wisp.present &&
                              std::memcmp(wisp.mac, p.sourceMac, 6) == 0 &&
                              (now - wisp.lastHelloMs) < kWispPairingWindowMs;
      if (!wispPaired) return;  // drop silently — defeat-the-defeat
    }
    PendingOverrideBrightness slot;
    std::memcpy(slot.sourceMac, p.sourceMac, 6);
    slot.surface = p.surface;
    slot.sourceKind = p.sourceKind;
    slot.fadeDurationMs = p.fadeDurationMs;
    slot.brightness = p.brightness;
    postPendingOverrideBrightness(slot);
  } else if (msgType == lamp_protocol::MSG_RESTORE_BRIGHTNESS) {
    lamp_protocol::ParsedRestoreBrightness p;
    if (!lamp_protocol::parseRestoreBrightness(data, len, p)) return;
    if (!restoreBrightnessDedup_.record(p.sourceMac,
                                        lamp_protocol::MSG_RESTORE_BRIGHTNESS,
                                        p.seq)) return;
    // No relay — see OVERRIDE_COLORS branch.
    if (!addressedToUs(p.targetMac, myMac_)) return;
    PendingRestoreBrightness slot;
    std::memcpy(slot.sourceMac, p.sourceMac, 6);
    slot.surface = p.surface;
    slot.sourceKind = p.sourceKind;
    slot.fadeDurationMs = p.fadeDurationMs;
    postPendingRestoreBrightness(slot);
  } else if (msgType == lamp_protocol::MSG_EVENT) {
    lamp_protocol::ParsedEvent ev;
    if (!lamp_protocol::parseEvent(data, len, ev)) return;
    // Dedup by (sourceMac, seq). The cascade sender emits two back-to-back
    // copies of MSG_EVENT for broadcast-loss resilience (ESP-NOW broadcasts
    // are not link-layer ACK'd); both share the same seq so the second
    // copy collapses here. Also collapses any future relay (we don't relay
    // MSG_EVENT today, but a peer's stack might).
    if (!eventDedup_.record(ev.sourceMac, lamp_protocol::MSG_EVENT, ev.seq)) return;
    // Drop our own broadcast — would otherwise re-trigger us locally.
    // The cascade originator is firing on its own clock already; no need
    // to round-trip through the wire.
    if (std::memcmp(ev.sourceMac, myMac_, 6) == 0) return;
    // v0x03 lock-in: gossip-relay MSG_EVENT.
    //
    // Old semantics (pre-v0x03): MSG_EVENT was single-hop. The cascade
    // originator's broadcast (×2 back-to-back) reached whatever was in
    // direct radio range, full stop. When the originator was BLE-coex'd
    // — common case: the app was open on the triggerer's phone — IDF
    // #14904's SW-coex packet loss dropped reliability to ~22% in the
    // field. No relay meant no other lamp could help.
    //
    // New semantics: every lamp that successfully receives a MSG_EVENT
    // for the first time (dedup gate above) AND isn't the originator
    // (self-MAC gate above) rebroadcasts the frame verbatim. Three things
    // bound the storm:
    //   1. eventDedup_.record() (64-slot ring per the v0x03 capacity bump)
    //      collapses any (sourceMac, seq) we've already seen. Total
    //      airborne copies per cascade ≤ N + 1 in an N-lamp mesh, not
    //      N² — each lamp emits at most one relay.
    //   2. The self-MAC gate at line 275 prevents the originator from
    //      re-firing its own gossip. The originator only emits its
    //      explicit broadcast pair from ExpressionManager::maybeCascade.
    //   3. Reach saturates at the dedup horizon: a frame arriving on
    //      lamp X after X already relayed it gets dropped before re-relay.
    //
    // Why BEFORE the eventKind filter below: forward-compat. If a future
    // EventKind (0x02..0xFF) ships on a newer fleet member, v0x03 lamps
    // still relay it (helping its propagation) while individually dropping
    // it from local application. That keeps the mesh from being a
    // single-version straightjacket — unknown-but-well-formed events still
    // hitchhike through the grid.
    //
    // The mode change from "best-effort sender re-emit" to "gossip-cascade
    // through all peers" is the primary cascade-reliability fix in the
    // production lock-in. Combined with HW coex (Commit F), targeted ≥90%
    // cascade reception on a 2-lamp BT-connected source.
    // why: cascade reliability fix per validated plan §"Layer 1".
    link_.broadcast(data, len);
    // Built-in event kinds: today only ExpressionTriggered. Drop unknown
    // kinds silently (forward-compat with user-defined kinds in 0x10+).
    // NOTE: this filter runs AFTER the gossip-relay above — see rationale.
    if (ev.eventKind != lamp_protocol::EventKind::ExpressionTriggered) return;
    // Look up our own MAC in the stagger entries. If present, take the
    // supplied delayMs; if absent (sender's peer list was truncated past
    // kMaxStaggerEntries, or we just joined the mesh between the sender's
    // last HELLO scrape and the cascade emit), tail-fire at
    // numStaggerEntries * kTailFireStaggerMs so we still participate but
    // don't pile onto the wavefront. 50 ms matches the typical
    // cascadeStaggerMs the app sets — a sensible default for "we missed
    // the wave; fall in behind."
    constexpr uint16_t kTailFireStaggerMs = 50;
    uint16_t delayMs = static_cast<uint16_t>(ev.numStaggerEntries) * kTailFireStaggerMs;
    for (uint8_t i = 0; i < ev.numStaggerEntries; ++i) {
      if (std::memcmp(ev.staggerEntries[i].mac, myMac_, 6) == 0) {
        delayMs = ev.staggerEntries[i].delayMs;
        break;
      }
    }
    // Payload is already capped at maxEventPayloadFor(numStaggerEntries)
    // by the parser; the PendingEvent.payload buffer is sized to the
    // zero-stagger best case so this memcpy can never overrun for any
    // valid frame. Defensive check anyway so a future protocol drift
    // can't silently overflow the slot.
    if (ev.payloadLen > lamp_protocol::maxEventPayloadFor(ev.numStaggerEntries)) return;
    PendingEvent slot;
    std::memcpy(slot.sourceMac, ev.sourceMac, 6);
    slot.delayMs = delayMs;
    slot.payloadLen = ev.payloadLen;
    if (ev.payloadLen && ev.payload) {
      std::memcpy(slot.payload, ev.payload, ev.payloadLen);
    }
    postPendingEvent(slot);
  } else if (msgType == lamp_protocol::MSG_FW_OFFER) {
    // Single-hop unicast; no gossip relay. The shared firmwareDedup_
    // ring guards against the wisp re-sending the OFFER while we drain.
    lamp_protocol::ParsedFwOffer p;
    if (!lamp_protocol::parseFwOffer(data, len, p)) return;
    if (!firmwareDedup_.record(p.sourceMac, lamp_protocol::MSG_FW_OFFER, p.seq)) return;
    if (!addressedToUs(p.targetMac, myMac_)) return;
    PendingFirmwareControl slot{};
    slot.msgType = lamp_protocol::MSG_FW_OFFER;
    slot.seq = p.seq;
    std::memcpy(slot.sourceMac, p.sourceMac, 6);
    std::memcpy(slot.targetMac, p.targetMac, 6);
    slot.offer.version       = p.version;
    slot.offer.totalLen      = p.totalLen;
    slot.offer.chunkSize     = p.chunkSize;
    std::memcpy(slot.offer.channel, p.channel, lamp_protocol::FW_CHANNEL_LEN);
    std::memcpy(slot.offer.sha256Prefix, p.sha256Prefix,
                lamp_protocol::FW_SHA256_PREFIX_LEN);
    slot.offer.footerLen   = p.footerLen;
    slot.offer.totalChunks = p.totalChunks;
    postPendingFirmwareControl(slot);
  } else if (msgType == lamp_protocol::MSG_FW_CHUNK) {
    // High-frequency payload-carrying frame. Per lamp-side plan §5
    // (Option 1, confirmed by spike at 16.6% WiFi-task-busy / 0% drops):
    // direct handoff to FirmwareReceiver::handleChunkOnRecvTask on this
    // recv task. No pending slot — the single-slot pattern can't keep
    // up with 250 chunks/s and the slot's "newest writer wins" semantics
    // would silently drop most of the stream.
    //
    // The receiver's chunk handler is bounded: ~0.5 ms for the
    // esp_ota_write_with_offset call + a bitmap set under portMUX. No
    // heap, no JSON, no FreeRTOS blocking.
    lamp_protocol::ParsedFwChunk p;
    if (!lamp_protocol::parseFwChunk(data, len, p)) return;
    if (!firmwareDedup_.record(p.sourceMac, lamp_protocol::MSG_FW_CHUNK, p.seq)) return;
    if (!addressedToUs(p.targetMac, myMac_)) return;
    if (firmwareReceiver_) firmwareReceiver_->handleChunkOnRecvTask(p);
  } else if (msgType == lamp_protocol::MSG_FW_DONE) {
    lamp_protocol::ParsedFwDone p;
    if (!lamp_protocol::parseFwDone(data, len, p)) return;
    if (!firmwareDedup_.record(p.sourceMac, lamp_protocol::MSG_FW_DONE, p.seq)) return;
    if (!addressedToUs(p.targetMac, myMac_)) return;
    PendingFirmwareControl slot{};
    slot.msgType = lamp_protocol::MSG_FW_DONE;
    slot.seq = p.seq;
    std::memcpy(slot.sourceMac, p.sourceMac, 6);
    std::memcpy(slot.targetMac, p.targetMac, 6);
    slot.done.version  = p.version;
    slot.done.totalLen = p.totalLen;
    std::memcpy(slot.done.sha256Prefix, p.sha256Prefix,
                lamp_protocol::FW_SHA256_PREFIX_LEN);
    slot.done.footerLen = p.footerLen;
    postPendingFirmwareControl(slot);
  }
  // NOTE: MSG_FW_ACCEPT, MSG_FW_REQ, MSG_FW_RESULT are lamp→wisp; the
  // lamp does not receive them. No branches needed here.
}

void ShowReceiver::emitHello() {
  if (!config_) return;
  uint8_t shade[4] = {0, 0, 0, 0};
  uint8_t base[4]  = {0, 0, 0, 0};
  if (!config_->shade.colors.empty()) {
    const Color& c = config_->shade.colors[0];
    shade[0] = c.r; shade[1] = c.g; shade[2] = c.b; shade[3] = c.w;
  }
  if (!config_->base.colors.empty()) {
    size_t ac = config_->base.ac;
    if (ac >= config_->base.colors.size()) ac = 0;
    const Color& c = config_->base.colors[ac];
    base[0] = c.r; base[1] = c.g; base[2] = c.b; base[3] = c.w;
  }

  const std::string& name = config_->lamp.name;
  const size_t nameLen = name.size() > lamp_protocol::HELLO_MAX_NAME
                           ? lamp_protocol::HELLO_MAX_NAME
                           : name.size();

  uint8_t buf[lamp_protocol::HELLO_MAX_SIZE];
  size_t n = lamp_protocol::buildHello(buf, sizeof(buf), helloSeq_++, myMac_,
                                       shade, base, FIRMWARE_VERSION,
                                       name.data(), nameLen);
  if (n) {
    link_.broadcast(buf, n);
  }
}

}  // namespace lamp
