#include "standard_lamp.hpp"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Preferences.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "components/network/bluetooth.hpp"
#include "components/network/ble_control.hpp"
#include "components/network/nearby_lamps.hpp"
#include "components/network/show_receiver.hpp"
#include "components/network/wifi.hpp"
#include "behaviors/show_behavior.hpp"
#include "expressions/expression_manager.hpp"
#include "util/color.hpp"
#include "behaviors/configurator.hpp"
#include "behaviors/fade_out.hpp"
#include "behaviors/knockout.hpp"
#include "behaviors/social.hpp"
#include "config/config.hpp"
#include "core/animated_behavior.hpp"
#include "core/compositor.hpp"
#include "core/frame_buffer.hpp"
#include "globals.hpp"
#include "pending_json_slot.hpp"
#include "util/color.hpp"
#include "util/gradient.hpp"
#include "util/levels.hpp"

Adafruit_NeoPixel* shadeStrip = nullptr;
Adafruit_NeoPixel* baseStrip = nullptr;
Preferences prefs;

// Zero-allocation pending slots. BLE callbacks on the NimBLE host task
// (Core 0) only do a fixed-size memcpy under portMUX into these slots. The
// loop task (Core 1) drains them and does ALL heap work — JSON parsing,
// vector building, state mutation. Single-core memory pattern.
//
// The slot data shape (valid bit + length + bounded buffer + memcpy under
// portMUX) is templated as lamp::PendingJsonSlot<N> in
// src/lamps/pending_json_slot.hpp. Each post-helper below is a one-line
// forwarder; each drain block below uses slot.drain(mux, buf) to read.
// Audit fix #9 — collapses nine open-coded copies of the same idiom.

constexpr size_t MAX_PENDING_JSON = 256;
constexpr size_t MAX_PENDING_OP_JSON = 512;  // expression op payloads are larger

struct PendingKnockoutUpdate {
  bool valid = false;
  uint8_t pixel = 0;
  uint8_t brightness = 100;
};

volatile int8_t pendingBrightness = -1;
// Flag set from Core 0 (BLE callbacks) when the home-mode preview state
// changes — either the flag itself flipped, or homeMode.brightness was
// updated via CHAR_HOME_PREVIEW cmd 0x02. The loop task on Core 1 drains
// it and calls applyEffectiveBrightness so the strip transitions cleanly.
volatile bool pendingApplyEffectiveBrightness = false;
// Flag set from Core 0 (BLE ServerCallbacks::onDisconnect) when the phone
// walks away — forces a synchronous disposition NVS commit so the user's
// final slider value survives even if power is yanked before the debounce
// window elapses. Core 1 drain calls config.flushDispositionsNow().
// Audit finding #5 (NVS write amplification) — see config.hpp.
volatile bool pendingFlushDispositionsRequested = false;
lamp::PendingJsonSlot<MAX_PENDING_JSON> pendingBaseColorsJson;
lamp::PendingJsonSlot<MAX_PENDING_JSON> pendingShadeColorsJson;
PendingKnockoutUpdate pendingKnockout;
lamp::PendingJsonSlot<MAX_PENDING_OP_JSON> pendingExpressionOpJson;
lamp::PendingJsonSlot<MAX_PENDING_OP_JSON> pendingWifiOpJson;
lamp::PendingJsonSlot<MAX_PENDING_OP_JSON> pendingTestActionJson;
lamp::PendingJsonSlot<MAX_PENDING_OP_JSON> pendingRemoteOpJson;
// Disposition map writes from CHAR_SOCIAL_DISPOSITIONS land here from
// Core 0; the loop drain on Core 1 parses + persists via Config so NVS
// writes serialise with the settings_blob drain on the same core.
lamp::PendingJsonSlot<MAX_PENDING_OP_JSON> pendingSocialDispositionsJson;

// Pending triggerExpression invocations whose delayMs > 0 — drained from the
// loop task once millis() reaches fireAtMs. Loop-task only (the WiFi-task
// CONTROL_OP handler just posts JSON into pendingInboundOpJson, and the
// drain that parses it into this queue runs on the loop task), so no mutex.
struct DelayedInvocation {
  lamp::ExpressionInvocation inv;
  uint32_t fireAtMs;
  uint8_t srcMac[6];   // carried through so receiver-side coalesce still
                       // works when the actual fire is delayed.
};
static constexpr size_t MAX_PENDING_TRIGGERS = 16;
std::vector<DelayedInvocation> pendingTriggers;
// settings_blob processing moved to the loop drain so it serialises with
// the other pending-op drains (especially expressionOp) on the same core.
// Decrypt + auth still happen on Core 0's BLE callback; the merge +
// putString + fadeOut happens here. See drain block in loop().
lamp::PendingJsonSlot<MAX_PENDING_OP_JSON> pendingSettingsBlobJson;
// Slot for ESP-NOW-inbound CONTROL_OP payloads. ShowReceiver runs the recv
// callback on the WiFi task; it memcpys the payload here and the loop task
// drains + dispatches. Mirrors the BLE Core 0 → loop pattern.
// Uses the WithMac variant because the inbound path carries the sender's
// MAC alongside the payload (downstream coalesce keys on it).
lamp::PendingJsonSlotWithMac<MAX_PENDING_OP_JSON> pendingInboundOpJson;
portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// Thin forwarders into the template. Each slot's bounds check + mux
// discipline lives inside PendingJsonSlot::post — these helpers exist
// only so ble_control.cpp (and the ESP-NOW recv callback) can stay
// blissfully unaware of which slot a particular char's payload lands in.
void postPendingShadeColorsJson(const char* data, size_t len) { pendingShadeColorsJson.post(pendingMux, data, len); }
void postPendingBaseColorsJson(const char* data, size_t len)  { pendingBaseColorsJson.post(pendingMux, data, len); }
void postPendingBrightness(int8_t level) { pendingBrightness = level; }
void postPendingApplyEffectiveBrightness() { pendingApplyEffectiveBrightness = true; }
// Single-bit post called from the NimBLE host task (Core 0) inside
// ServerCallbacks::onDisconnect — NVS is NOT Core-0-safe so we cannot
// call config.flushDispositionsNow() there directly. The loop drain on
// Core 1 picks this up next iteration and runs the synchronous flush.
void postPendingFlushDispositions() { pendingFlushDispositionsRequested = true; }
void postPendingKnockout(uint8_t pixel, uint8_t brightness) {
  portENTER_CRITICAL(&pendingMux);
  pendingKnockout.pixel = pixel;
  pendingKnockout.brightness = brightness;
  pendingKnockout.valid = true;
  portEXIT_CRITICAL(&pendingMux);
}

void postPendingExpressionOpJson(const char* data, size_t len)      { pendingExpressionOpJson.post(pendingMux, data, len); }
void postPendingWifiOpJson(const char* data, size_t len)            { pendingWifiOpJson.post(pendingMux, data, len); }
void postPendingTestActionJson(const char* data, size_t len)        { pendingTestActionJson.post(pendingMux, data, len); }
void postPendingSettingsBlobJson(const char* data, size_t len)      { pendingSettingsBlobJson.post(pendingMux, data, len); }
void postPendingRemoteOpJson(const char* data, size_t len)          { pendingRemoteOpJson.post(pendingMux, data, len); }
void postPendingSocialDispositionsJson(const char* data, size_t len){ pendingSocialDispositionsJson.post(pendingMux, data, len); }

lamp::BluetoothComponent bt;
lamp::Compositor compositor;
lamp::FrameBuffer shade;
lamp::FrameBuffer base;
lamp::SocialBehavior shadeSocialBehavior;
lamp::ConfiguratorBehavior shadeConfiguratorBehavior;
lamp::ConfiguratorBehavior baseConfiguratorBehavior;
lamp::FadeOutBehavior shadeFadeOutBehavior;
lamp::FadeOutBehavior baseFadeOutBehavior;
lamp::KnockoutBehavior baseKnockoutBehavior;
lamp::ExpressionManager expressionManager;
lamp::Config config;
lamp::ShowReceiver showReceiver;
lamp::ShowBehavior shadeShowBehavior;
lamp::ShowBehavior baseShowBehavior;

// ----- Local appliers for mesh-received ops --------------------------------
//
// The drain blocks in loop() and the mesh receive path (applyRemoteOpLocal)
// both end up performing the same mutation: walk a JSON colors array into a
// std::vector<Color>, build a gradient, push it into the configurator
// behavior, update the BLE advert, invalidate the section. Likewise for
// expressionOp upsert/remove.
//
// These helpers factor that mutation out so applyRemoteOpLocal — which
// already has a parsed JsonDocument in hand — can apply the change directly
// instead of re-serializing the colors array into a std::string just so the
// drain can re-parse it. Audit fix: eliminates parse → serialize → memcpy →
// parse round-trip on every inbound mesh shadeColors/baseColors/expressionOp.
//
// INVARIANT: these run on the loop task (Core 1). They touch compositor
// behaviors, bluetooth advert state, and config sections — none of which is
// Core-0-safe. The BLE host-task path stays on the post-to-pending-slot
// pattern (see ble_control.cpp WriteRouter instances); the slot drain on
// Core 1 calls these helpers after parsing the buffered bytes.

// Walk a JsonArray of hex strings into a std::vector<Color>. Shared between
// the two color helpers below.
static std::vector<lamp::Color> jsonArrayToColors(JsonArray arr) {
  std::vector<lamp::Color> colors;
  colors.reserve(arr.size());
  for (JsonVariant v : arr) {
    colors.push_back(lamp::hexStringToColor(v));
  }
  return colors;
}

// Apply a shadeColors update locally. Used by both the pendingShadeColorsJson
// drain (Core 1) and applyRemoteOpLocal (also Core 1). `arr` must be a
// non-empty JSON array of hex color strings. Callers handle the bookkeeping
// (configurator timestamps + section invalidate) themselves so the drain
// can preserve its prior unconditional behavior on parse-failure / empty-
// array inputs without this helper having to guess at it.
static void applyShadeColorsLocal(JsonArray arr) {
  if (arr.isNull() || arr.size() == 0) return;
  std::vector<lamp::Color> colors = jsonArrayToColors(arr);
  shadeConfiguratorBehavior.colors =
      lamp::buildGradientWithStops(shade.pixelCount, colors);
  // Reflect the new shade in the BLE adv so phones and v1 neighbours see it
  // without having to connect. Use the first stop — shade in this build is
  // a single color.
  bt.setAdvertisedColors(config.base.colors[config.base.ac], colors[0]);
}

// Apply a baseColors update locally. Counterpart to applyShadeColorsLocal.
// See the bookkeeping note above — callers handle timestamps + invalidate.
static void applyBaseColorsLocal(JsonArray arr) {
  if (arr.isNull() || arr.size() == 0) return;
  std::vector<lamp::Color> colors = jsonArrayToColors(arr);
  baseConfiguratorBehavior.colors =
      lamp::buildGradientWithStops(base.pixelCount, colors);
  // Reflect the new base in the BLE adv — first stop is what the adv carries
  // (we don't know the user's active-stop index from this drain, and the
  // first stop is what bt.begin used initially).
  bt.setAdvertisedColors(colors[0], config.shade.colors[0]);
}

// Apply an expressionOp upsert/remove locally. `doc` carries `op` plus the
// op-specific payload (`entry` for upsert, `type`+`target` for remove).
// Mirrors the manager state into config.expressions so the next
// settings_blob save persists the runtime edit.
static void applyExpressionOpLocal(JsonObject doc) {
  if (doc.isNull()) return;
  const char* op = doc["op"].as<const char*>();
  if (op && strcmp(op, "upsert") == 0 && doc["entry"].is<JsonObject>()) {
    JsonObject entry = doc["entry"].as<JsonObject>();
    lamp::ExpressionConfig cfg;
    cfg.type = std::string(entry["type"] | "");
    cfg.enabled = entry["enabled"] | true;
    cfg.intervalMin = entry["intervalMin"] | 60;
    cfg.intervalMax = entry["intervalMax"] | 900;
    cfg.target = entry["target"] | 3;
    for (JsonPair kv : entry) {
      std::string key(kv.key().c_str());
      if (key == "type" || key == "enabled" || key == "intervalMin" ||
          key == "intervalMax" || key == "target" || key == "colors") continue;
      JsonVariant v = kv.value();
      if (v.is<uint32_t>()) cfg.setParameter(key, v.as<uint32_t>());
      else if (v.is<int>()) cfg.setParameter(key, static_cast<uint32_t>(v.as<int>()));
    }
    // Store the JsonArray in a local so iteration doesn't reference a
    // temporary that's destroyed at the end of the full expression
    // (ArduinoJson 7.4.x tightened lifetime semantics on chained calls;
    // before this the same code happened to work).
    JsonArray colorsArr = entry["colors"].as<JsonArray>();
    for (JsonVariant cv : colorsArr) {
      cfg.colors.push_back(lamp::hexStringToColor(cv));
    }
    if (!cfg.type.empty()) {
      expressionManager.upsertExpression(cfg, &compositor);
      // Mirror into config.expressions so the next settings_blob save
      // persists the user's latest edits. expressionManager is the runtime
      // animator; config is what gets serialized to NVS.
      auto& exprs = config.expressions.expressions;
      bool found = false;
      for (auto& e : exprs) {
        if (e.type == cfg.type && e.target == cfg.target) {
          e = cfg;
          found = true;
          break;
        }
      }
      if (!found) exprs.push_back(cfg);
    }
  } else if (op && strcmp(op, "remove") == 0) {
    const char* type = doc["type"].as<const char*>();
    int tgt = doc["target"] | 0;
    if (type && tgt >= 1 && tgt <= 3) {
      expressionManager.removeExpression(type, static_cast<lamp::ExpressionTarget>(tgt), &compositor);
      // Mirror removal into config.expressions.
      auto& exprs = config.expressions.expressions;
      exprs.erase(std::remove_if(exprs.begin(), exprs.end(),
                    [&](const lamp::ExpressionConfig& e) {
                      return e.type == type && e.target == tgt;
                    }),
                  exprs.end());
    }
  }
  // NOTE: callers invalidate the expressions section themselves so that the
  // drain block preserves its prior behavior of always marking dirty (even
  // on parse failure where nothing actually changed).
}

// Apply a remote-op payload locally (either from BLE remoteOp drain when
// targetMac==self/broadcast, or from an incoming ESP-NOW MSG_CONTROL_OP).
// Both call sites run on the loop task (Core 1) — the BLE remoteOp drain
// runs in loop() directly, and the ESP-NOW path goes via ShowReceiver's
// WiFi-task handler which only does memcpy into pendingInboundOpJson; the
// loop drain then calls this function. Because we're already on Core 1, we
// mutate state directly via the applyXxxLocal helpers above instead of
// re-serializing into a pending slot just so a drain can re-parse it.
//
// `payload` must be a NUL-terminated JSON string for ArduinoJson to parse.
// `srcMac` identifies the sender (used by triggerInvocation to coalesce
// rapid same-sender cascades). For BLE-initiated paths the caller passes
// `myMac_` so app-driven triggers coalesce against each other.
static void applyRemoteOpLocal(const char* payloadJson, size_t len,
                               const uint8_t srcMac[6]) {
  JsonDocument doc;
  if (deserializeJson(doc, payloadJson, len) != DeserializationError::Ok) return;
  const char* ch = doc["char"].as<const char*>();
  if (!ch || !*ch) return;

  if (strcmp(ch, "brightness") == 0) {
    int level = doc["value"] | -1;
    if (level >= 0 && level <= 100) postPendingBrightness(static_cast<int8_t>(level));

  } else if (strcmp(ch, "shadeColors") == 0) {
    // Direct path: applyRemoteOpLocal runs on Core 1, so call the local
    // applier with the JsonArray we already have. Skips the slot round-trip
    // (audit: was serializeJson → std::string → memcpy → drain → re-parse).
    applyShadeColorsLocal(doc["colors"].as<JsonArray>());
    // Match the drain's unconditional bookkeeping.
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    config.invalidateShadeSection();

  } else if (strcmp(ch, "baseColors") == 0) {
    applyBaseColorsLocal(doc["colors"].as<JsonArray>());
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    config.invalidateBaseSection();

  } else if (strcmp(ch, "knockout") == 0) {
    int pixel = doc["pixel"] | -1;
    int brightness = doc["brightness"] | -1;
    if (pixel >= 0 && pixel < 256 && brightness >= 0 && brightness <= 100) {
      postPendingKnockout(static_cast<uint8_t>(pixel), static_cast<uint8_t>(brightness));
    }

  } else if (strcmp(ch, "expressionOp") == 0) {
    // Direct path: same Core 1 reasoning — call the applier with the parsed
    // JsonObject. The drain shape expects the `char` key gone, but the
    // applier just looks at `op`/`entry`/`type`/`target`, so leaving `char`
    // is harmless. Skips serialize → drain → re-parse.
    applyExpressionOpLocal(doc.as<JsonObject>());
    // Match the drain's unconditional invalidate semantics.
    config.invalidateExpressionsSection();

  } else if (strcmp(ch, "triggerExpression") == 0) {
    // Receive side of the mesh expression-trigger primitive. The payload IS
    // the invocation (the `char` key is the only wrapper). We never re-emit
    // — that's the structural loop break.
    lamp::ExpressionInvocation inv;
    if (!lamp::parseInvocation(doc.as<JsonObjectConst>(), inv)) return;
    if (inv.delayMs == 0) {
      expressionManager.triggerInvocation(inv, srcMac);
    } else {
      if (pendingTriggers.size() >= MAX_PENDING_TRIGGERS) {
        // FIFO eviction — most-recent intent wins. Dropping the newest
        // would lose the user's latest command in a mesh storm.
#ifdef LAMP_DEBUG
        Serial.println("[loop] triggerExpression queue full, evicting oldest");
#endif
        pendingTriggers.erase(pendingTriggers.begin());
      }
      DelayedInvocation d;
      d.inv = inv;
      d.fireAtMs = millis() + inv.delayMs;
      memcpy(d.srcMac, srcMac, 6);
      pendingTriggers.push_back(d);
    }
  }
  // settings forwarding is intentionally deferred — it triggers a remote
  // reboot whose UX over the grid needs more thought. Follow-up plan.
}

// Origin transport for the unified cascade-receive router. The two paths
// differ only in (a) how `srcMac` is acquired and (b) where the "is this
// for us / should we forward" decision is taken. See applyRemoteOpRouted.
enum class RemoteOpTransport { BLE, EspNow };

// Unified cascade-receive router. Single place where the BLE remoteOp drain
// and the ESP-NOW CONTROL_OP receive path converge — both used to duplicate
// the self-vs-broadcast logic, the ApplyX dispatch, and the forwarding
// policy. Audit MEDIUM finding (two parallel cascade-receive paths).
//
// Inputs:
//   `payloadJson`  : NUL-terminated JSON. For BLE this is the raw GATT
//                    write payload; for ESP-NOW it's the CONTROL_OP payload
//                    that ShowReceiver already passed through MAC + dedup
//                    filtering on the WiFi task.
//   `srcMac`       : Sender's MAC. For ESP-NOW this is the original CONTROL_OP
//                    `sourceMac`. For BLE there is no real network source —
//                    the caller passes selfMac so app-driven rapid triggers
//                    coalesce against each other on the receive side.
//   `origin`       : Selects the small bit of asymmetry between the two
//                    transports (see comments below). Everything else —
//                    JSON dispatch (`applyRemoteOpLocal`), RecentCascade
//                    dedup + suppressCascade_ invariant (both internal to
//                    ExpressionManager and consulted via triggerInvocation),
//                    delayed-trigger queue, settings-not-forwarded policy —
//                    is shared.
//
// What still differs between BLE and EspNow, handled here:
//   - For BLE the payload arrives wrapped with a top-level `targetMac`
//     ("broadcast" | "AA:BB:..."). The router strips it, decides self /
//     broadcast / unicast, applies locally when self-or-broadcast, and
//     forwards over ESP-NOW (broadcast or unicast) when not-self.
//   - For EspNow the addressed-to-us check (`forUs`) and the once-only
//     grid rebroadcast already happened on the WiFi task inside
//     ShowReceiver::handleRecv (kept there for latency: rebroadcast doesn't
//     wait on the loop drain). By the time the slot is drained the payload
//     is unconditionally for-us, so the router just applies locally.
//
// settings forwarding is still intentionally deferred — same comment as in
// applyRemoteOpLocal. Preserving prior behavior; not in scope to fix here.
static void applyRemoteOpRouted(const char* payloadJson, size_t len,
                                const uint8_t srcMac[6],
                                RemoteOpTransport origin) {
  if (origin == RemoteOpTransport::EspNow) {
    // ShowReceiver::handleRecv (WiFi task) already gated on targetMac == myMac
    // || broadcast, and already rebroadcast once for grid relay. Nothing for
    // the router to decide — just dispatch locally on Core 1.
    applyRemoteOpLocal(payloadJson, len, srcMac);
    return;
  }

  // BLE origin. The CHAR_REMOTE_OP drain handed us a payload that may carry a
  // top-level `targetMac` field selecting self / broadcast / a specific peer.
  JsonDocument doc;
  if (deserializeJson(doc, payloadJson, len) != DeserializationError::Ok) return;

  const char* tgtStr = doc["targetMac"].as<const char*>();
  uint8_t targetMac[6] = {0};
  bool isBroadcast = false;
  bool isSelf = false;
  if (tgtStr) {
    if (strcmp(tgtStr, "broadcast") == 0) {
      memset(targetMac, 0xFF, 6);
      isBroadcast = true;
    } else if (sscanf(tgtStr, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
                      &targetMac[0], &targetMac[1], &targetMac[2],
                      &targetMac[3], &targetMac[4], &targetMac[5]) == 6) {
      uint8_t myMac[6];
      showReceiver.getMyMac(myMac);
      isSelf = (memcmp(targetMac, myMac, 6) == 0);
    }
  }

  // Strip targetMac before applying/forwarding — applyRemoteOpLocal and the
  // CONTROL_OP payload both expect the unwrapped op shape.
  doc.remove("targetMac");
  std::string payload;
  serializeJson(doc, payload);

  if (isSelf || isBroadcast) {
    // applyRemoteOpLocal's srcMac is the *original* sender for the cascade
    // coalesce. For BLE-initiated ops the "sender" is this lamp's own app
    // session — `srcMac` was already populated with selfMac by the caller.
    applyRemoteOpLocal(payload.data(), payload.size(), srcMac);
  }
  if (!isSelf) {
    // Forward over ESP-NOW. broadcast => fan out to all peers; unicast =>
    // targets the specific MAC. ShowReceiver::sendControlOp also records
    // its own seq into controlOpDedup_ so the rebroadcast we'll get back
    // doesn't loop in as an "apply locally".
    showReceiver.sendControlOp(
        targetMac,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size());
  }
}

// Forward decl — defined later, alongside effectiveBrightness which
// shares the same gate. initBehaviors uses it to seed compositor.begin.
static bool calculateEffectiveHomeMode();

void initBehaviors() {
  shadeSocialBehavior = lamp::SocialBehavior(&shade, 1200);
  // Live config pointer so SocialBehavior::control reads the current
  // socialMode each tick (user can change personality at runtime; the
  // change rides through settings_blob save + reboot, but the wiring
  // is per-instance regardless).
  shadeSocialBehavior.setConfig(&config);
  // Pause social greetings when the lamp is in home mode — home mode is
  // the user's "I'm home, calm down" mode. Compositor gates this via
  // the homeMode flag, kept in sync by reapplyHomeModeState().
  shadeSocialBehavior.allowedInHomeMode = false;
  shadeConfiguratorBehavior = lamp::ConfiguratorBehavior(&shade, 120);
  shadeConfiguratorBehavior.colors = shade.defaultColors;
  baseConfiguratorBehavior = lamp::ConfiguratorBehavior(&base, 120);
  baseConfiguratorBehavior.colors = base.defaultColors;
  shadeFadeOutBehavior = lamp::FadeOutBehavior(&shade, REBOOT_ANIMATION_FRAMES);
  baseFadeOutBehavior = lamp::FadeOutBehavior(&base, REBOOT_ANIMATION_FRAMES);
  baseKnockoutBehavior = lamp::KnockoutBehavior(&base, 0, true);
  baseKnockoutBehavior.knockoutPixels = config.base.knockoutPixels;

  // ShowBehaviors render the latest COLORS frame received via ESP-NOW. They
  // only run while ShowReceiver reports a fresh frame; otherwise they stay
  // STOPPED and lower-priority behaviors (expressions, idle) render.
  shadeShowBehavior = lamp::ShowBehavior(&shade, 0, true);
  shadeShowBehavior.setSide(lamp::ShowBehavior::SHADE);
  shadeShowBehavior.setReceiver(&showReceiver);
  baseShowBehavior = lamp::ShowBehavior(&base, 0, true);
  baseShowBehavior.setSide(lamp::ShowBehavior::BASE);
  baseShowBehavior.setReceiver(&showReceiver);

  expressionManager.begin(&shade, &base);
  expressionManager.loadFromConfig(config.expressions);

  std::vector<lamp::AnimatedBehavior*> allBehaviors = {};

  // Expression behaviors (lowest priority — automated effects)
  auto exprBehaviors = expressionManager.getBehaviors();
  allBehaviors.insert(allBehaviors.end(), exprBehaviors.begin(), exprBehaviors.end());

  // ShowBehaviors sit just above expressions: when a grid COLORS frame is
  // fresh they take over; when it ages out they yield to expressions.
  allBehaviors.push_back(&shadeShowBehavior);
  allBehaviors.push_back(&baseShowBehavior);

  // Social greeting behaviors (high priority)
  allBehaviors.push_back(&shadeSocialBehavior);

  // Configurator behaviors (highest priority — UI preview)
  allBehaviors.push_back(&baseConfiguratorBehavior);
  allBehaviors.push_back(&shadeConfiguratorBehavior);

  // Fade-out behaviors run last so reboot animation is on top of everything
  allBehaviors.push_back(&baseFadeOutBehavior);
  allBehaviors.push_back(&shadeFadeOutBehavior);

  compositor.begin(allBehaviors, {&shade, &base}, calculateEffectiveHomeMode());
  // Record where the initial expression behaviors end so runtime adds insert
  // before higher-priority behaviors (social, configurator, fade-out).
  compositor.setExpressionBandEnd(exprBehaviors.size());
  compositor.overlayBehaviors.push_back(&baseKnockoutBehavior);

  // Finish wiring the shared BehaviorContext. The Compositor self-publishes
  // in its constructor; we publish the ExpressionManager + frame buffer list
  // here so the expressions just registered by compositor.begin() can reach
  // both from this point onward. (setCompositor() later in setup() repeats
  // these writes idempotently — they're cheap pointer assignments.)
  auto& behaviorCtx = compositor.behaviorContext();
  behaviorCtx.expressionManager = &expressionManager;
  behaviorCtx.expressionFrameBuffers.clear();
  behaviorCtx.expressionFrameBuffers.push_back(&shade);
  behaviorCtx.expressionFrameBuffers.push_back(&base);
}

/**
 * Parse ExpressionConfig from JSON object using generic parameter system
 */
lamp::ExpressionConfig parseExpressionConfig(JsonObject node) {
  lamp::ExpressionConfig expr;
  expr.type = std::string(node["type"] | "");
  expr.enabled = node["enabled"] | false;
  expr.intervalMin = node["intervalMin"] | 60;
  expr.intervalMax = node["intervalMax"] | 900;
  expr.target = node["target"] | 3;

  JsonArray colors = node["colors"];
  if (colors.size()) {
    for (JsonVariant color : colors) {
      expr.colors.push_back(lamp::hexStringToColor(color));
    }
  }

  for (JsonPair kv : node) {
    const char* key = kv.key().c_str();
    std::string keyStr(key);
    if (keyStr == "type" || keyStr == "enabled" || keyStr == "intervalMin" ||
        keyStr == "intervalMax" || keyStr == "target" || keyStr == "colors") {
      continue;
    }
    JsonVariant value = kv.value();
    if (value.is<uint32_t>()) {
      expr.setParameter(keyStr, value.as<uint32_t>());
    } else if (value.is<int>()) {
      expr.setParameter(keyStr, static_cast<uint32_t>(value.as<int>()));
    }
  }

  return expr;
}

void dispatchLampAction(JsonDocument& doc, unsigned long updateTimeMs) {
  shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = updateTimeMs;
  baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = updateTimeMs;

  String action = String(doc["a"]);
  if (action == "test_expression") {
    String type = String(doc["type"]);
    if (type.length() > 0) {
      lamp::ExpressionTarget target = doc["target"].is<int>()
        ? static_cast<lamp::ExpressionTarget>(doc["target"].as<int>())
        : lamp::TARGET_BOTH;
#ifdef LAMP_DEBUG
      auto colors = expressionManager.getExpressionColors(type.c_str());
      String colorList;
      for (const auto& c : colors) {
        if (colorList.length() > 0) colorList += " ";
        colorList += lamp::colorToHexString(c).c_str();
      }
      Serial.printf("Testing expression: %s target=%d [%s]\n",
                    type.c_str(), static_cast<int>(target), colorList.c_str());
#endif
      shadeConfiguratorBehavior.disabled = true;
      baseConfiguratorBehavior.disabled = true;
      expressionManager.triggerExpression(type.c_str(), target);
    }
  } else if (action == "test_expression_complete") {
    shadeConfiguratorBehavior.disabled = false;
    baseConfiguratorBehavior.disabled = false;
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();

    if (doc["shadeColors"]) {
      JsonArray shadeColors = doc["shadeColors"];
      if (shadeColors.size()) {
        std::vector<lamp::Color> updatedColors;
        for (JsonVariant shadeColor : shadeColors) {
          updatedColors.push_back(lamp::hexStringToColor(shadeColor));
        }
        shadeConfiguratorBehavior.colors = lamp::buildGradientWithStops(shade.pixelCount, updatedColors);
      }
    }
    if (doc["baseColors"]) {
      JsonArray baseColors = doc["baseColors"];
      if (baseColors.size()) {
        std::vector<lamp::Color> updatedColors;
        for (JsonVariant baseColor : baseColors) {
          updatedColors.push_back(lamp::hexStringToColor(baseColor));
        }
        baseConfiguratorBehavior.colors = lamp::buildGradientWithStops(base.pixelCount, updatedColors);
      }
    }
  }
}

extern void lamp_register_panic_handler();

// Effective brightness, mirroring calculateEffectiveHomeMode() so the
// brightness value and the compositor's homeMode gate stay in lockstep.
// See calculateEffectiveHomeMode below for the rule.
static uint8_t effectiveBrightness();
static bool calculateEffectiveHomeMode();
static uint8_t effectiveBrightness() {
  return calculateEffectiveHomeMode() ? config.homeMode.brightness
                                      : config.lamp.brightness;
}

static void applyEffectiveBrightness() {
  uint8_t level = effectiveBrightness();
  if (shadeStrip) shadeStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
  if (baseStrip) baseStrip->setBrightness(lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
}

// Two regimes:
//   1. BT client connected (the app is the "configurator"): home mode is
//      forced OFF unless the user is on the Home Mode page, in which case
//      it's forced ON so they can preview brightness / behavior changes.
//      The flag is set by the app via CHAR_HOME_MODE_FOCUS and cleared on
//      BT disconnect.
//   2. No BT client connected: presence-based. Home mode iff the user
//      has enabled it AND has a saved SSID AND the most recent wifi scan
//      saw that SSID nearby. The lamp never associates — just sniffs
//      beacons. No password ever leaves the lamp.
static bool calculateEffectiveHomeMode() {
  if (ble_control::isClientConnected()) {
    return ble_control::isHomeModePageActive();
  }
  return config.homeMode.enabled
      && !config.homeMode.ssid.empty()
      && wifi::homeSsidVisible(config.homeMode.ssid);
}

// Single funnel for "home mode state may have changed" — keeps the
// compositor's behavior gate and the strip brightness in lockstep so
// the lamp transitions cleanly when preview flips or WiFi associates /
// disassociates.
static void reapplyHomeModeState() {
  compositor.setHomeMode(calculateEffectiveHomeMode());
  applyEffectiveBrightness();
}

static void onWifiStateChanged() {
  // This callback fires from Arduino-ESP32's WiFi event task — NOT Core 1.
  // Calling into compositor.setHomeMode / shadeStrip->setBrightness from
  // here races Core 1's compositor.tick + frame_buffer.flush, corrupting
  // the NeoPixel byte buffer and the behavior vector. Symptom: lamp
  // crash-loops with rst:0x3 (SW_RESET) + _invalid_pc_placeholder during
  // background scan completion or any other WiFi state transition.
  //
  // Safe path: post the pending flag and let Core 1's loop drain call
  // reapplyHomeModeState on its own thread.
  postPendingApplyEffectiveBrightness();
  ble_control::notifyWifiState();
}

void setup() {
#ifdef LAMP_DEBUG
  Serial.begin(115200);
#endif
  lamp_register_panic_handler();
  config = lamp::Config(&prefs);

  wifi::begin();
  wifi::setStateChangeCallback(onWifiStateChanged);

  bt.begin(config.lamp.name, config.base.colors[config.base.ac], config.shade.colors[0]);
  bt.activateGattServices(&config, &prefs);

  // Map the section's byteOrder string to the NeoPixel format flag. The
  // bpp-derived fallback covers lamps that didn't carry the new field in
  // their NVS payload (see config.cpp's loader — byteOrder is back-filled
  // there, so this branch shouldn't fire in practice).
  auto pickStripFmt = [](const std::string& order, uint8_t bpp) -> uint16_t {
    if (order == "GRBW") return NEO_GRBW;
    if (order == "GRB")  return NEO_GRB;
    if (order == "BGR")  return NEO_BGR;
    return (bpp == 4) ? NEO_GRBW : NEO_GRB;
  };
  const uint16_t shadeFmt = pickStripFmt(config.shade.byteOrder, config.shade.bpp);
  const uint16_t baseFmt  = pickStripFmt(config.base.byteOrder,  config.base.bpp);
  shadeStrip = new Adafruit_NeoPixel(LAMP_MAX_STRIP_PIXELS_SHADE, LAMP_SHADE_PIN, shadeFmt + NEO_KHZ800);
  baseStrip = new Adafruit_NeoPixel(LAMP_MAX_STRIP_PIXELS_BASE, LAMP_BASE_PIN, baseFmt + NEO_KHZ800);
  applyEffectiveBrightness();
  shade.begin(lamp::buildGradientWithStops(config.shade.px, config.shade.colors), config.shade.px, shadeStrip);
  base.begin(lamp::buildGradientWithStops(config.base.px, config.base.colors), config.base.px, baseStrip);
  initBehaviors();

  // Presence-only home mode — the lamp never associates to an AP. The
  // radio sits on LAMP_ESPNOW_CHANNEL (set in wifi::begin) for grid
  // peers, and a periodic background scan in wifi::tick checks whether
  // the user's saved home SSID is currently visible.
  wifi::ensureGridChannel();

  // Route inbound CONTROL_OP payloads (addressed to us or broadcast) into a
  // pending slot. WiFi-task safe: pure memcpy under portMUX, no heap work.
  // MUST be installed BEFORE showReceiver.begin() — otherwise any CONTROL_OP
  // that arrives in the gap is dropped because controlOpHandler_ is null.
  showReceiver.setControlOpHandler(
      [](const uint8_t* payload, size_t len, const uint8_t srcMac[6]) {
        pendingInboundOpJson.post(
            pendingMux, reinterpret_cast<const char*>(payload), len, srcMac);
      });
  // Bring up ESP-NOW grid presence (HELLO + COLORS). Independent of home
  // WiFi — runs on whatever channel the radio is on. See lamp_protocol.hpp.
  showReceiver.begin(&config);
  // Wire the cascade fan-out path. The manager only sends after this; before
  // begin/setShowReceiver, local triggers fire but never cascade.
  expressionManager.setShowReceiver(&showReceiver);
  // Compositor wired so the manager can lazy-upsert a transient entry when a
  // remote cascade arrives for an expression type that's not configured on
  // this lamp — receivers no longer need to pre-configure every type.
  expressionManager.setCompositor(&compositor);
  // One-shot reservation so the loop-task drain never reallocates mid-frame.
  pendingTriggers.reserve(MAX_PENDING_TRIGGERS);

};

void loop() {
  // Drain pending BLE actions on the loop task (Core 1). All heap allocation
  // (JsonDocument parse, std::vector, gradient construction) happens here,
  // NOT in BLE callbacks on Core 0.

  // Debounced flush of any pending BLE advertisement color update. The
  // drain blocks below call bt.setAdvertisedColors() freely (it's a
  // fast cache write); the actual NimBLE setAdvertisementData() call
  // is rate-limited inside tickAdvertising() to avoid the host-task
  // race that panics the lamp on rapid color picker drags.
  bt.tickAdvertising();

  if (pendingApplyEffectiveBrightness) {
    pendingApplyEffectiveBrightness = false;
    // Preview enter/exit (cmd 0x01/0x00) and live home-brightness writes
    // (cmd 0x02) all funnel here — refresh the compositor homeMode gate
    // and the strip brightness together.
    reapplyHomeModeState();
  }

  // Audit fix #5: NVS write amplification on disposition slider drag.
  // The eager persist inside Config::setDisposition was replaced with a
  // dirty-flag + timestamp; this poll runs the actual commit when the
  // user has been idle for kDispositionFlushIdleMs (5s). Cheap when
  // nothing is dirty — single bool check + uint32_t subtraction.
  config.maybeFlushDispositions(millis());
  if (pendingFlushDispositionsRequested) {
    // Phone disconnected (set on Core 0 in ble_control's onDisconnect).
    // Force-commit so the user's final slider value survives even if
    // power is yanked before the next 5s idle window would fire.
    pendingFlushDispositionsRequested = false;
    config.flushDispositionsNow();
  }

  if (pendingBrightness >= 0) {
    uint8_t level = static_cast<uint8_t>(pendingBrightness);
    pendingBrightness = -1;
#ifdef LAMP_DEBUG
    Serial.printf("[drain] brightness=%u t_us=%lu home_focus=%d\n",
                  (unsigned)level, (unsigned long)micros(),
                  (int)ble_control::isHomeModePageActive());
#endif
    // Route the write to home.brightness vs lamp.brightness based on
    // which page the app is on. When the user is configuring home mode
    // we want the slider to set the home value; otherwise it sets the
    // regular lamp brightness.
    if (ble_control::isHomeModePageActive()) {
      config.homeMode.brightness = level;
    } else {
      config.lamp.brightness = level;
    }
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    // Apply the wire value directly — the user just wrote `level`, so
    // by definition that IS the effective brightness right now. Skipping
    // the effectiveBrightness() lookup keeps this hot path off the
    // wifi::homeSsidVisible string-compare loop, matching the pre-
    // simplification behavior (commit 7023f29) where this drain was
    // smooth under continuous slider drag.
    if (shadeStrip) shadeStrip->setBrightness(
        lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
    if (baseStrip)  baseStrip->setBrightness(
        lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
    // Invalidate both — brightness can land in either section depending
    // on whether the app is on the Home Mode page. Two bools is cheap.
    config.invalidateLampSection();
    config.invalidateHomeSection();
  }

  if (pendingShadeColorsJson.valid) {
    char buf[MAX_PENDING_JSON + 1];
    uint16_t len = pendingShadeColorsJson.drain(pendingMux, buf);

#ifdef LAMP_DEBUG
    Serial.printf("[drain] shadeColors len=%u t_us=%lu\n",
                  (unsigned)len, (unsigned long)micros());
#else
    (void)len;
#endif

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      applyShadeColorsLocal(doc.as<JsonArray>());
    }
    // Preserve the prior drain's unconditional bookkeeping — timestamps +
    // invalidate were already outside the parse-OK / array-non-empty guards,
    // and keeping them that way avoids a behavior delta.
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    config.invalidateShadeSection();
  }

  if (pendingBaseColorsJson.valid) {
    char buf[MAX_PENDING_JSON + 1];
    uint16_t len = pendingBaseColorsJson.drain(pendingMux, buf);

#ifdef LAMP_DEBUG
    Serial.printf("[drain] baseColors len=%u t_us=%lu\n",
                  (unsigned)len, (unsigned long)micros());
#else
    (void)len;
#endif

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      applyBaseColorsLocal(doc.as<JsonArray>());
    }
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    config.invalidateBaseSection();
  }

  if (pendingKnockout.valid) {
    uint8_t pixel, brightness;
    portENTER_CRITICAL(&pendingMux);
    pixel = pendingKnockout.pixel;
    brightness = pendingKnockout.brightness;
    pendingKnockout.valid = false;
    portEXIT_CRITICAL(&pendingMux);
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain knockout pixel=%u brightness=%u\n", pixel, brightness);
#endif
    if (pixel < config.base.px && brightness <= 100) {
      baseKnockoutBehavior.knockoutPixels[pixel] = brightness;
      config.base.knockoutPixels[pixel] = brightness;
      config.invalidateBaseSection();
    }
  }

  if (pendingExpressionOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len = pendingExpressionOpJson.drain(pendingMux, buf);

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain expressionOp len=%u\n", (unsigned)len);
#else
    (void)len;
#endif

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      applyExpressionOpLocal(doc.as<JsonObject>());
    }
    config.invalidateExpressionsSection();
  }

  // settings_blob drain — runs AFTER expressionOp drain so that any
  // just-arrived expression edits are already mirrored into
  // config.expressions before we serialize and persist. Uses
  // config.asJsonDocument() as the base (current canonical state) and
  // overlays the incoming partial-blob's top-level keys; anything the
  // app omits is preserved from current state.
  if (pendingSettingsBlobJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len = pendingSettingsBlobJson.drain(pendingMux, buf);

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain settingsBlob len=%u\n", (unsigned)len);
#endif

    JsonDocument incomingDoc;
    if (deserializeJson(incomingDoc, buf, len) != DeserializationError::Ok) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] settingsBlob: incoming JSON parse failed\n");
#endif
    } else if (incomingDoc["factoryReset"].as<bool>()) {
      // Factory reset sentinel — wipe the lamp's NVS namespace entirely
      // (clears both `cfg` AND `dispositions` keys) and reboot. Comes up
      // with empty NVS → Config defaults → awaiting adoption. App
      // auth-gates this write when a password is set; an unprovisioned
      // lamp accepts it but has nothing to lose by being wiped.
#ifdef LAMP_DEBUG
      Serial.println("[loop] settingsBlob: factoryReset sentinel, wiping NVS");
#endif
      // Audit fix: guard both prefs.begin AND prefs.clear. If begin returns
      // false (NVS full / partition corrupt) we must NOT putString/clear
      // against an unopened handle, and we must NOT flag a reboot — leaving
      // the lamp running with its previous state is better than rebooting
      // into an unknown half-cleared state. Same for a clear() that returns
      // false: skip the reboot, keep current state, log and move on.
      if (!prefs.begin("lamp", false)) {
#ifdef LAMP_DEBUG
        Serial.println("[nvs] prefs.begin failed (factory reset)");
#endif
      } else {
        bool cleared = prefs.clear();
        prefs.end();
        if (cleared) {
          ble_control::notifyStateChange();
          lamp::fadeOutRebootRequested = true;
        } else {
#ifdef LAMP_DEBUG
          Serial.println("[nvs] prefs.clear failed; skipping reboot flag");
#endif
        }
      }
    } else {
      JsonDocument fullDoc = config.asJsonDocument();
      JsonObject full = fullDoc.as<JsonObject>();
      // One-level-deep nested merge for object-valued top-level keys:
      // when the app ships a partial section (e.g. `base` with colors
      // but no knockout, because knockout is too big for the BLE ATT
      // cap), preserve the fields it didn't ship by overlaying the
      // incoming fields onto the existing section object rather than
      // wholesale-replacing it. Without this, `config.base.knockout`
      // gets clobbered on every save and pixel-knockout doesn't
      // survive a power cycle. Arrays + scalars still replace.
      for (JsonPair kv : incomingDoc.as<JsonObject>()) {
        if (kv.value().is<JsonObject>() && full[kv.key()].is<JsonObject>()) {
          JsonObject dst = full[kv.key()].as<JsonObject>();
          for (JsonPair inner : kv.value().as<JsonObject>()) {
            dst[inner.key()] = inner.value();
          }
        } else {
          full[kv.key()] = kv.value();
        }
      }
      String mergedJson;
      serializeJson(fullDoc, mergedJson);

      // Audit fix: capture prefs.begin() — if it returns false (NVS full or
      // partition corrupt) putString writes silently to nothing. Skip the
      // putString and do NOT flag a reboot; the user's settings stay in
      // RAM and the next save attempt may succeed.
      if (!prefs.begin("lamp", false)) {
#ifdef LAMP_DEBUG
        Serial.println("[nvs] prefs.begin failed (settings_blob persist)");
#endif
      } else {
        size_t written = prefs.putString("cfg", mergedJson.c_str());
        prefs.end();

        if (written > 0) {
#ifdef LAMP_DEBUG
          Serial.printf("[loop] settingsBlob: persisted %u bytes, fading out for reboot\n",
                        (unsigned)written);
#endif
          ble_control::notifyStateChange();
          lamp::fadeOutRebootRequested = true;
        } else {
#ifdef LAMP_DEBUG
          Serial.printf("[loop] settingsBlob: putString failed\n");
#endif
        }
      }
    }
    // settings_blob can rewrite any subset of sections; cheapest correct
    // path is to invalidate all and let ble_control::tick() rebuild only
    // what gets actually read on the next BLE round trip.
    config.invalidateAllSections();
  }

  // Disposition map writes — drained AFTER settings_blob so both writers
  // serialise against the shared `prefs` instance on this core. The BLE
  // callback only memcpys into the pending slot (Core 0); persistence +
  // map rebuild happen here on Core 1. No reboot, no auth re-check — the
  // BLE callback already verified isAuthed before posting.
  if (pendingSocialDispositionsJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len = pendingSocialDispositionsJson.drain(pendingMux, buf);
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain socialDispositions len=%u\n", (unsigned)len);
#endif
    config.setDispositionsFromJson(buf, len);
  }

  if (pendingTestActionJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len = pendingTestActionJson.drain(pendingMux, buf);

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain testAction len=%u\n", (unsigned)len);
#endif

    JsonDocument doc;
    if (len == 0) {
      doc["a"] = "test_expression_complete";
      dispatchLampAction(doc, millis());
    } else {
      DeserializationError err = deserializeJson(doc, buf, len);
      const char* action = err ? nullptr : doc["a"].as<const char*>();
      if (action && *action) {
        dispatchLampAction(doc, millis());
      } else {
        doc.clear();
        std::string value(buf, len);
        if (value == "complete") {
          doc["a"] = "test_expression_complete";
        } else {
          doc["a"] = "test_expression";
          doc["type"] = value;
        }
        dispatchLampAction(doc, millis());
      }
    }
  }

  if (pendingWifiOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len = pendingWifiOpJson.drain(pendingMux, buf);

#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain wifiOp len=%u\n", (unsigned)len);
#endif

    JsonDocument doc;
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
      const char* op = doc["op"].as<const char*>();
      if (op && strcmp(op, "scan") == 0) {
        wifi::startScan();
      }
      // setHomeSsid + forget moved to the unified draft + settings_blob
      // path. The app holds the SSID locally and persists it via the blob
      // along with everything else — wifiOp is now scan-only.
    }
  }

  // Drain inbound ESP-NOW CONTROL_OP (deferred from ShowReceiver's WiFi
  // task) — JSON parse + local dispatch via the unified cascade router.
  if (pendingInboundOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint8_t srcMac[6];
    uint16_t len = pendingInboundOpJson.drain(pendingMux, buf, srcMac);
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain inboundOp len=%u\n", (unsigned)len);
#endif
    applyRemoteOpRouted(buf, len, srcMac, RemoteOpTransport::EspNow);
  }

  // Drain BLE CHAR_REMOTE_OP writes through the same router — it decides
  // apply-locally / forward / both from the payload's targetMac field.
  // BLE has no real network source MAC; pass selfMac so app-driven rapid
  // triggers coalesce against each other on the receive side.
  if (pendingRemoteOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len = pendingRemoteOpJson.drain(pendingMux, buf);
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain remoteOp len=%u\n", (unsigned)len);
#endif
    uint8_t selfMac[6];
    showReceiver.getMyMac(selfMac);
    applyRemoteOpRouted(buf, len, selfMac, RemoteOpTransport::BLE);
  }

  // Fire any delayed triggerExpression invocations whose deadline has passed.
  // Bounded queue (MAX_PENDING_TRIGGERS) keeps this O(1) amortised; ordering
  // is INSERTION-order, not deadline-order — if a short-delayMs invocation
  // arrives after a long-delayMs one, the long one fires first when its
  // deadline hits. Best-effort by design; the cascade UX tolerates the
  // jitter and avoiding a priority queue keeps the drain trivial.
  if (!pendingTriggers.empty()) {
    const uint32_t now = millis();
    for (auto it = pendingTriggers.begin(); it != pendingTriggers.end();) {
      if (static_cast<int32_t>(now - it->fireAtMs) >= 0) {
        expressionManager.triggerInvocation(it->inv, it->srcMac);
        it = pendingTriggers.erase(it);
      } else {
        ++it;
      }
    }
  }

  wifi::tick();
  showReceiver.tick();
  // Rebuild + push any dirty section JSON to its NimBLE characteristic
  // so onRead callbacks (Core 0) hand back NimBLE's already-buffered
  // bytes without walking config vectors. Cheap when nothing's dirty.
  ble_control::tick();

  compositor.tick();

  // Reap transient one-shot expressions (created by triggerInvocation when
  // a remote cascade arrived) whose animations have finished. AFTER tick so
  // the final frame of the animation is drawn before removal.
  expressionManager.gcTransients();
};
