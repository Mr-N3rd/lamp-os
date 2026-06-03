#include "./show_receiver.hpp"

#include <Arduino.h>

#include <algorithm>
#include <cstring>

namespace lamp {

ShowReceiver* ShowReceiver::s_instance = nullptr;

void ShowReceiver::onRecv(const uint8_t* mac, const uint8_t* data, size_t len) {
  if (s_instance) s_instance->handleRecv(mac, data, len);
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

// Serialize + size-check + send. Returns false (and logs in debug) when the
// payload would exceed the ~230-byte ESP-NOW CONTROL_OP cap so we don't fail
// silently inside sendControlOp's own bounds check.
static bool sendInvocationToMac(ShowReceiver& self, const uint8_t mac[6],
                                const ExpressionInvocation& inv) {
  std::string json;
  serializeInvocation(inv, json);
  if (json.size() > lamp_protocol::CONTROL_MAX_PAYLOAD) {
#ifdef LAMP_DEBUG
    Serial.printf("[show] triggerExpression payload %u > max %u, dropping\n",
                  (unsigned)json.size(),
                  (unsigned)lamp_protocol::CONTROL_MAX_PAYLOAD);
#endif
    return false;
  }
  return self.sendControlOp(mac,
                            reinterpret_cast<const uint8_t*>(json.data()),
                            json.size());
}

bool ShowReceiver::sendExpressionTo(const std::string& peerName,
                                    const ExpressionInvocation& inv) {
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
    return sendInvocationToMac(*this, p.mac, inv);
  }
#ifdef LAMP_DEBUG
  Serial.printf("[show] sendExpressionTo: peer '%s' not reachable\n",
                peerName.c_str());
#endif
  return false;
}

void ShowReceiver::sendExpressionToAll(const ExpressionInvocation& inv,
                                       uint32_t staggerMs) {
  auto peers = nearbyLamps.getReachableViaEspNow(LAMP_PRUNE_TIME_MS);
  std::vector<NearbyLamp> targets;
  targets.reserve(peers.size());
  for (const auto& p : peers) {
    if (!p.hasMac) continue;
    if (std::memcmp(p.mac, myMac_, 6) == 0) continue;  // never send to self
    targets.push_back(p);
  }
  // Deterministic name order so the cascade direction is stable visit-to-visit
  // (otherwise NearbyLamps internal insertion order would leak in).
  std::sort(targets.begin(), targets.end(),
            [](const NearbyLamp& a, const NearbyLamp& b) {
              return a.name < b.name;
            });
  for (size_t i = 0; i < targets.size(); i++) {
    ExpressionInvocation perPeer = inv;
    perPeer.delayMs = inv.delayMs + static_cast<uint32_t>(i) * staggerMs;
    sendInvocationToMac(*this, targets[i].mac, perPeer);
  }
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

bool ShowReceiver::hasRecentFrame(uint32_t maxAgeMs) {
  bool have;
  uint32_t lastMs;
  portENTER_CRITICAL(&mux_);
  have = haveFrame_;
  lastMs = latestFrameMs_;
  portEXIT_CRITICAL(&mux_);
  if (!have) return false;
  return (millis() - lastMs) <= maxAgeMs;
}

bool ShowReceiver::snapshot(uint8_t shade[4], uint8_t base[4], uint8_t* mode, uint8_t* parameter) {
  bool have;
  portENTER_CRITICAL(&mux_);
  have = haveFrame_;
  if (have) {
    std::memcpy(shade, latestShade_, 4);
    std::memcpy(base, latestBase_, 4);
    if (mode) *mode = latestMode_;
    if (parameter) *parameter = latestParam_;
  }
  portEXIT_CRITICAL(&mux_);
  return have;
}

void ShowReceiver::handleRecv(const uint8_t* /*srcMac*/, const uint8_t* data, size_t len) {
  const uint8_t msgType = lamp_protocol::inspect(data, len);
  if (msgType == lamp_protocol::MSG_COLORS) {
    lamp_protocol::ParsedColors c;
    if (!lamp_protocol::parseColors(data, len, c)) return;
    // Dedup by (targetMac, seq) — also gates whether to rebroadcast.
    if (!colorsDedup_.record(c.targetMac, lamp_protocol::MSG_COLORS, c.seq)) return;

    // Rebroadcast unseen COLORS once for gossip range extension.
    link_.broadcast(data, len);

    // If addressed to us, latch the state.
    if (std::memcmp(c.targetMac, myMac_, 6) == 0) {
      portENTER_CRITICAL(&mux_);
      std::memcpy(latestShade_, c.shade, 4);
      std::memcpy(latestBase_,  c.base,  4);
      latestMode_ = c.mode;
      latestParam_ = c.parameter;
      latestFrameMs_ = millis();
      haveFrame_ = true;
      portEXIT_CRITICAL(&mux_);
    }
  } else if (msgType == lamp_protocol::MSG_HELLO) {
    lamp_protocol::ParsedHello h;
    if (!lamp_protocol::parseHello(data, len, h)) return;
    if (!helloDedup_.record(h.sourceMac, lamp_protocol::MSG_HELLO, h.seq)) return;
    // Don't rebroadcast our OWN hellos.
    if (std::memcmp(h.sourceMac, myMac_, 6) == 0) return;
    // Record into the unified nearbyLamps store so the BLE CHAR_NEARBY_LAMPS
    // read + SocialBehavior + any other consumer all see this lamp.
    const std::string peerName = h.nameLen ? std::string(h.name, h.nameLen) : std::string();
    nearbyLamps.addOrUpdateFromEspNow(
        peerName,
        h.sourceMac,
        Color(h.base[0],  h.base[1],  h.base[2],  h.base[3]),
        Color(h.shade[0], h.shade[1], h.shade[2], h.shade[3]));
    link_.broadcast(data, len);
  } else if (msgType == lamp_protocol::MSG_CONTROL_OP) {
    lamp_protocol::ParsedControlOp op;
    if (!lamp_protocol::parseControlOp(data, len, op)) return;
    // Dedup by (sourceMac, seq) so a loop-relayed copy doesn't fire twice.
    if (!controlOpDedup_.record(op.sourceMac, lamp_protocol::MSG_CONTROL_OP, op.seq)) return;
    // Rebroadcast for grid relay (terminates at peers that have seen this seq).
    link_.broadcast(data, len);
    // Apply locally if addressed to us or broadcast.
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const bool forUs = (std::memcmp(op.targetMac, myMac_, 6) == 0) ||
                       (std::memcmp(op.targetMac, bcast, 6) == 0);
    if (forUs && controlOpHandler_) {
#ifdef LAMP_DEBUG
      Serial.printf("[show] CONTROL_OP apply len=%u\n", (unsigned)op.payloadLen);
#endif
      controlOpHandler_(op.payload, op.payloadLen);
    }
  }
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
                                       shade, base, name.data(), nameLen);
  if (n) {
    link_.broadcast(buf, n);
  }
}

}  // namespace lamp
