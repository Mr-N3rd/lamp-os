#include "standard_lamp.hpp"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Preferences.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "components/firmware/firmware_receiver.hpp"
#include "components/firmware/firmware_distributor.hpp"
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_ota_ops.h>
#endif
#include "components/network/bluetooth.hpp"
#include "components/network/ble_control.hpp"
#include "components/network/nearby_lamps.hpp"
#include "components/network/show_receiver.hpp"
#include "components/network/wifi.hpp"
#include "components/personality/personality_engine.hpp"
#include "components/transient_override/brightness_override.hpp"
#include "components/transient_override/color_override.hpp"
#include "expressions/expression_manager.hpp"
#include "behaviors/configurator.hpp"
#include "behaviors/fade_out.hpp"
#include "behaviors/knockout.hpp"
#include "behaviors/personality.hpp"
#include "behaviors/social.hpp"
#include "config/config.hpp"
#include "config/config_types.hpp"
#include "core/animated_behavior.hpp"
#include "core/behavior_context.hpp"
#include "core/compositor.hpp"
#include "core/frame_buffer.hpp"
#include "pending_json_slot.hpp"
#include "pending_typed_slot.hpp"
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

// ── User brightness micro-fade ─────────────────────────────────────────────
// The pendingBrightness drain (live-preview from CHAR_BRIGHTNESS) used to
// call shadeStrip->setBrightness directly on every write. Each WriteCoalescer
// window delivers a write every ~60-100 ms, so a sustained slider drag landed
// as visibly stepped brightness changes — user described it as "not smooth".
//
// This small fade state interpolates between successive user writes over
// kUserBrightnessFadeMs (~80 ms — slightly longer than the coalescer floor so
// each step bleeds into the next without overshooting). Drain stamps a new
// (source, target, start) triple per write; the loop tick interpolates and
// applies. Tick is gated to brightnessOverride.isActive() == false so an
// active wisp / personality override owns the strip uncontested via the
// existing applyEffectiveBrightness change-callback path.
//
// Source initialisation: drain reads the current visual level by lerping the
// previous fade in flight, so an immediate re-write picks up wherever the
// strip actually is. Cold-start (no prior write) seeds source = target so the
// very first write snaps without a black-up ramp.
static constexpr uint16_t kUserBrightnessFadeMs = 80;
static uint8_t  s_userBrightnessSource = 0;
static uint8_t  s_userBrightnessTarget = 0;
static uint32_t s_userBrightnessFadeStartMs = 0;
static bool     s_userBrightnessSeeded = false;

// Compute the current interpolated brightness for the live user-write fade.
// Returns target once the fade window has elapsed.
static uint8_t computeUserBrightnessNow(uint32_t nowMs) {
  if (s_userBrightnessSource == s_userBrightnessTarget) {
    return s_userBrightnessTarget;
  }
  const uint32_t elapsed = nowMs - s_userBrightnessFadeStartMs;
  if (elapsed >= kUserBrightnessFadeMs) {
    return s_userBrightnessTarget;
  }
  const int32_t span =
      static_cast<int32_t>(s_userBrightnessTarget) - s_userBrightnessSource;
  return static_cast<uint8_t>(
      s_userBrightnessSource +
      (span * static_cast<int32_t>(elapsed)) /
          static_cast<int32_t>(kUserBrightnessFadeMs));
}
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
// Phase D — app-originated wispOp writes (CHAR_WISP_OP). Drain on Core 1
// broadcasts as MSG_CONTROL_OP. Routed through a DEDICATED slot rather
// than the converged applyRemoteOpLocal so that gossip-relayed wispOps
// don't get reinterpreted on every lamp — wispOp is wisp-only, lamps
// only forward.
//
// SECURITY (accepted threat T1): wispOp `setWifi` carries the home WiFi
// PSK as plaintext JSON. The MSG_CONTROL_OP broadcast we emit from the
// drain is also plaintext — the wisp has no shared key with the lamp
// to decrypt with, so the only real fix is fleet-wide mesh auth (a
// shared PSK distributed at provisioning). That was deliberately
// rejected. See docs/superpowers/notes/2026-06-10-accepted-security-
// threats.md on the app side for the full rationale. Threat scope:
// passive ESP-NOW sniffer in mesh range on LAMP_ESPNOW_CHANNEL during
// a setWifi op — bounded by physical proximity at configuration time.
lamp::PendingJsonSlot<MAX_PENDING_OP_JSON> pendingWispOpJson;
// Phase D — wispStatus payloads observed on the mesh (either own BLE
// inbound or gossip-relayed from another lamp). applyRemoteOpLocal
// recognizes char:"wispStatus" and posts here; the drain caches into
// NearbyLamps and notifies BLE subscribers. The MAC slot carries the
// wisp's own MAC (source of the original CONTROL_OP) so cacheWispStatus
// knows whose status it is.
lamp::PendingJsonSlotWithMac<MAX_PENDING_OP_JSON> pendingWispStatusJson;
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
// Phase C transient-override pending slots. ShowReceiver's WiFi recv path
// memcpys the POD payload here; the Core 1 loop drain dispatches into the
// override module. Same Core 0 → Core 1 hand-off shape as the BLE slots
// above, but typed via PendingTypedSlot<T> instead of JSON byte buffer.
lamp::PendingTypedSlot<lamp::PendingOverrideColors>     pendingOverrideColors;
lamp::PendingTypedSlot<lamp::PendingRestoreColors>      pendingRestoreColors;
lamp::PendingTypedSlot<lamp::PendingOverrideBrightness> pendingOverrideBrightness;
lamp::PendingTypedSlot<lamp::PendingRestoreBrightness>  pendingRestoreBrightness;
lamp::PendingTypedSlot<lamp::PendingWispHello>          pendingWispHello;
lamp::PendingTypedSlot<lamp::PendingWispPalette>        pendingWispPalette;
// MSG_EVENT slot — ShowReceiver's WiFi recv path resolves the per-peer
// delayMs from the stagger list, memcpys the payload here, and the Core 1
// drain hands off to expressionManager.tryHandleExpressionEvent which does
// the JSON peek + RecentCascade dedup + (optional pendingTriggers enqueue).
// No local-config consult: cascade is sender-authoritative.
lamp::PendingTypedSlot<lamp::PendingEvent>              pendingEvent;
// Phase F: OFFER/DONE control-plane handoff. ShowReceiver's WiFi recv
// branches populate this slot; the loop drain on Core 1 calls
// firmwareReceiver.handleControlOnLoop(). MSG_FW_CHUNK does NOT go
// through a slot — it's handled directly on the WiFi task by the
// FirmwareReceiver (per the lamp-side plan §5 Option 1, spike-confirmed
// at 16.6% WiFi-task-busy / 0% drops).
lamp::PendingTypedSlot<lamp::PendingFirmwareControl>    pendingFirmwareControl;
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
// Phase D wispOp: app → CHAR_WISP_OP → here → drain → MSG_CONTROL_OP
// broadcast. The slot carries no mac because the drain inserts the
// broadcast target itself.
void postPendingWispOpJson(const char* data, size_t len)            { pendingWispOpJson.post(pendingMux, data, len); }
// Phase D wispStatus: applyRemoteOpLocal sees char:"wispStatus" and
// memcpys here under portMUX along with the original wisp's MAC.
// The drain caches via NearbyLamps + emits a BLE notify.
static void postPendingWispStatusJson(const uint8_t srcMac[6],
                                      const char* data, size_t len) {
  pendingWispStatusJson.post(pendingMux, data, len, srcMac);
}

namespace lamp {
// Phase C: typed-payload posts called from ShowReceiver's WiFi recv task.
// Same Core 0 → Core 1 hand-off discipline as the JSON slots above —
// memcpy under portMUX, no heap, no parsing on the recv task.
void postPendingOverrideColors(const PendingOverrideColors& src)         { pendingOverrideColors.post(pendingMux, src); }
void postPendingRestoreColors(const PendingRestoreColors& src)           { pendingRestoreColors.post(pendingMux, src); }
void postPendingOverrideBrightness(const PendingOverrideBrightness& src) { pendingOverrideBrightness.post(pendingMux, src); }
void postPendingRestoreBrightness(const PendingRestoreBrightness& src)   { pendingRestoreBrightness.post(pendingMux, src); }
void postPendingWispHello(const PendingWispHello& src)                   { pendingWispHello.post(pendingMux, src); }
void postPendingWispPalette(const PendingWispPalette& src)               { pendingWispPalette.post(pendingMux, src); }
void postPendingEvent(const PendingEvent& src)                           { pendingEvent.post(pendingMux, src); }
void postPendingFirmwareControl(const PendingFirmwareControl& src)       { pendingFirmwareControl.post(pendingMux, src); }

// Forwarder used by ExpressionManager::tryHandleExpressionEvent (and the
// legacy triggerExpression CONTROL_OP recv path) to schedule a future
// triggerInvocation without each call site having to know the queue
// storage lives here. Runs on Core 1 (drain task); the pendingTriggers
// vector is loop-task-only so no mutex needed. FIFO eviction on overflow
// — most-recent intent wins, dropping the newest would lose the user's
// latest cascade in a mesh storm.
void enqueueDelayedInvocation(const ExpressionInvocation& inv,
                              const uint8_t srcMac[6],
                              uint32_t delayMs) {
  if (pendingTriggers.size() >= MAX_PENDING_TRIGGERS) {
#ifdef LAMP_DEBUG
    Serial.println("[loop] delayed-trigger queue full, evicting oldest");
#endif
    pendingTriggers.erase(pendingTriggers.begin());
  }
  DelayedInvocation d;
  d.inv = inv;
  d.fireAtMs = millis() + delayMs;
  std::memcpy(d.srcMac, srcMac, 6);
  pendingTriggers.push_back(d);
}
}  // namespace lamp

lamp::BluetoothComponent bt;
lamp::Compositor compositor;
lamp::FrameBuffer shade;
lamp::FrameBuffer base;
lamp::SocialBehavior shadeSocialBehavior;
lamp::PersonalityBehavior shadePersonalityBehavior;
lamp::ConfiguratorBehavior shadeConfiguratorBehavior;
lamp::ConfiguratorBehavior baseConfiguratorBehavior;
lamp::FadeOutBehavior shadeFadeOutBehavior;
lamp::FadeOutBehavior baseFadeOutBehavior;
lamp::KnockoutBehavior baseKnockoutBehavior;
lamp::ExpressionManager expressionManager;
lamp::Config config;
lamp::ShowReceiver showReceiver;
// Phase F: lamp-side OTA receiver. Bound to showReceiver via
// setFirmwareReceiver() in setup() so the WiFi recv path can hand
// MSG_FW_CHUNK directly to handleChunkOnRecvTask (Core 0). OFFER/DONE
// arrive via pendingFirmwareControl and are drained on Core 1.
lamp::FirmwareReceiver    firmwareReceiver;
// firmwareDistributor's global instance lives in firmware_distributor.cpp
// (extern decl in firmware_distributor.hpp) so SocialBehavior can reference
// it as `lamp::firmwareDistributor` from any TU — matches the pattern used
// by `lamp::nearbyLamps` and `lamp::personalityEngine`.

// Phase G: post-OTA "mark this app valid" state.
//
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1 ships in pioarduino's prebuilt
// sdkconfig. After the first OTA reboot, the new partition is in
// ESP_OTA_IMG_PENDING_VERIFY state; a second esp_ota_begin against that
// partition returns ESP_ERR_OTA_ROLLBACK_INVALID_STATE — every subsequent
// OTA fails until the new firmware calls
// esp_ota_mark_app_valid_cancel_rollback().
//
// We arm a 30-second timer in setup() iff we boot in pending-verify
// state. The loop tick checks the timer; on fire, we re-confirm basic
// health (BLE running, loop has iterated) and call mark_app_valid.
// Otherwise: do nothing — the bootloader will revert on the next reset.
bool g_pendingVerify = false;
uint32_t g_markValidDeadlineMs = 0;
// Phase C transient overrides. Three instances: one ColorOverride per
// surface (base + shade), and a single BrightnessOverride that maps the
// whole-lamp brightness (Surface::Any in v1). bind() wires them to their
// configurator via the BehaviorContext after compositor.begin().
lamp::ColorOverride baseColorOverride;
lamp::ColorOverride shadeColorOverride;
lamp::BrightnessOverride brightnessOverride;

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
  std::vector<lamp::Color> gradient =
      lamp::buildGradientWithStops(shade.pixelCount, colors);
  // Phase C: drive the change through beginFade() so the BLE color picker
  // keeps its fade UX (~250ms ease). Mid-fade interrupts (rapid writes
  // during a drag) are handled inside ConfiguratorBehavior::beginFade —
  // it computes the in-progress lerp value as the new fade-from endpoint,
  // so successive writes rubber-band smoothly without re-snapshotting the
  // post-overlay buffer (which on Base contains knockout dimming and
  // would otherwise flicker on dimmed pixels).
  shadeConfiguratorBehavior.beginFade(gradient, lamp::kDefaultFadeMs);
  shadeColorOverride.rebaseline(gradient);
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
  std::vector<lamp::Color> gradient =
      lamp::buildGradientWithStops(base.pixelCount, colors);
  // See applyShadeColorsLocal: the fade-snapshot-from-buffer flicker on
  // knockout pixels is now handled at the ConfiguratorBehavior::beginFade
  // call site itself (snapshots from `colors` / in-progress lerp, not
  // fb->buffer). No mid-fade guard needed here.
  baseConfiguratorBehavior.beginFade(gradient, lamp::kDefaultFadeMs);
  baseColorOverride.rebaseline(gradient);
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
      // `disabledDuringWispOverride` is tolerated from older app payloads
      // but ignored — it's now a pure type-property on the Expression
      // subclass, not config-driven.
      if (key == "type" || key == "enabled" ||
          key == "disabledDuringWispOverride" ||
          key == "intervalMin" || key == "intervalMax" ||
          key == "target" || key == "colors") continue;
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
    // DO NOT invalidate the shade section here. See the architectural
    // comment at the CHAR_BRIGHTNESS drain (~line 954) for why live
    // preview must not poison the persisted-snapshot section cache.

  } else if (strcmp(ch, "baseColors") == 0) {
    applyBaseColorsLocal(doc["colors"].as<JsonArray>());
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    // Same as shadeColors above — live preview must not poison the
    // persisted-snapshot section cache.

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
    // Receive side of the single-peer-named triggerExpression CONTROL_OP.
    // The MSG_EVENT broadcast cascade has its own recv path through
    // tryHandleExpressionEvent and does NOT route through here. We never
    // re-emit — that's the structural loop break.
    lamp::ExpressionInvocation inv;
    if (!lamp::parseInvocation(doc.as<JsonObjectConst>(), inv)) return;
    if (inv.delayMs == 0) {
      expressionManager.triggerInvocation(inv, srcMac);
    } else {
      lamp::enqueueDelayedInvocation(inv, srcMac, inv.delayMs);
    }

  } else if (strcmp(ch, "wispStatus") == 0) {
    // Phase D: a wispStatus payload reached us — either directly from the
    // wisp's MSG_CONTROL_OP broadcast or via a peer's gossip relay. We
    // don't APPLY anything locally (lamps don't have a zone state to
    // mutate); we just cache + notify so the phone-paired lamp's
    // CHAR_WISP_STATUS read/notify surface stays current. srcMac is the
    // wisp's MAC (preserved through gossip relay by ShowReceiver, which
    // copies op.sourceMac into the inbound slot).
    postPendingWispStatusJson(srcMac, payloadJson, len);
  }
  // wispOp intentionally has NO branch here. The dedicated CHAR_WISP_OP
  // slot drain broadcasts a MSG_CONTROL_OP; the wisp(s) on the mesh
  // consume it. A gossip-relayed wispOp lands here, finds no matching
  // branch, and silently drops — which is exactly the desired behavior.
  // See docs/superpowers/plans/2026-06-04-wisp-phase-d-zone-selection.md
  // ("Don't add wispOp to applyRemoteOpLocal naively") for the design.

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
  if (!isSelf && !showReceiver.isOtaInProgress()) {
    // Forward over ESP-NOW. broadcast => fan out to all peers; unicast =>
    // targets the specific MAC. ShowReceiver::sendControlOp also records
    // its own seq into controlOpDedup_ so the rebroadcast we'll get back
    // doesn't loop in as an "apply locally".
    //
    // Suppressed during gossip OTA: control-op forwards compete with the
    // chunk stream for ESP-NOW airtime under BLE coex. Dropped forwards
    // are recoverable — the app retries on its next loop drain — but a
    // dropped chunk needs a stall-watchdog REQ round trip to recover.
    showReceiver.sendControlOp(
        targetMac,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size());
  }
}

// Forward decl — defined later, alongside effectiveBrightness which
// shares the same gate. initBehaviors uses it to seed compositor.begin.
static bool calculateEffectiveHomeMode();
// Forward decl — same reasoning. The Phase C BrightnessOverride callback
// wired in initBehaviors needs to reach the same brightness applier the
// rest of the lamp uses. Defined below alongside effectiveBrightness.
static void applyEffectiveBrightness();
static uint8_t effectiveBrightness();

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

  // PersonalityBehavior — additive shade-tint layer fed by
  // PersonalityEngine::getBleedContributions. Perpetually-playing so its
  // draw runs every frame. allowedInHomeMode=true so the subtle Fond bleed
  // stays visible even in the user's calm-down mode (it's a quiet social
  // signal, not a loud effect).
  shadePersonalityBehavior = lamp::PersonalityBehavior(&shade, /*frames=*/1);
  shadePersonalityBehavior.allowedInHomeMode = true;

  expressionManager.begin(&shade, &base);
  expressionManager.loadFromConfig(config.expressions);

  std::vector<lamp::AnimatedBehavior*> allBehaviors = {};

  // Behavior draw order = registration order, last-writer-wins on the
  // surface buffer.
  //
  // Configurator (wisp paint + saved colors) goes FIRST — it's the base
  // scene. Personality bleeds on top so Fond peer colors read against
  // the painted baseline. Social greetings overlay next. Expressions
  // come LAST so brief transient effects (glitchy / pulse / breathing /
  // shifty) compose on top of everything else and naturally yield when
  // their animation completes (animationState=STOPPED → Compositor
  // skips them → configurator's writes are the final state).
  //
  // Pre-2026-06-12 expressions were registered FIRST, which meant the
  // configurator overwrote their per-pixel writes every frame. Non-
  // exclusive expressions were effectively invisible during wisp paint.
  // The test_expression handler had a `disabled` flag on the configurator
  // to work around this for preview UX. Both went away with this
  // reorder — expressions now compose correctly without any "disable
  // the base layer" hack.

  // Configurator (base scene — saved colors + wisp paint via beginFade)
  allBehaviors.push_back(&baseConfiguratorBehavior);
  allBehaviors.push_back(&shadeConfiguratorBehavior);

  // Personality bleed — alpha-blend of Fond peer colors on top of base.
  allBehaviors.push_back(&shadePersonalityBehavior);

  // Social greeting behaviors
  allBehaviors.push_back(&shadeSocialBehavior);

  // Expression behaviors LAST — transient effects compose on top of
  // everything below. When their animationState transitions to STOPPED
  // the compositor skips them and the configurator's base scene shows
  // through (no recovery needed).
  auto exprBehaviors = expressionManager.getBehaviors();
  allBehaviors.insert(allBehaviors.end(), exprBehaviors.begin(), exprBehaviors.end());

  // Fade-out behaviors run last so reboot animation is on top of everything
  allBehaviors.push_back(&baseFadeOutBehavior);
  allBehaviors.push_back(&shadeFadeOutBehavior);

  compositor.begin(allBehaviors, {&shade, &base}, calculateEffectiveHomeMode());
  // Mark where the initial expression band ends so runtime-added transient
  // invocations are inserted into the expression block (preserves "all
  // expressions draw together, late in the list" ordering).
  compositor.setExpressionBandEnd(allBehaviors.size() - 2);  // before fade-out behaviors
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
  // Phase C: publish the two configurator pointers so the per-surface
  // ColorOverride instances can resolve their target configurator via
  // bind() without grabbing globals.
  behaviorCtx.baseConfigurator = &baseConfiguratorBehavior;
  behaviorCtx.shadeConfigurator = &shadeConfiguratorBehavior;
  // bind() the override instances. From here on apply()/restore() will
  // drive the right configurator's beginFade.
  baseColorOverride.bind(behaviorCtx, lamp_protocol::OverrideSurface::Base);
  shadeColorOverride.bind(behaviorCtx, lamp_protocol::OverrideSurface::Shade);
  // Wisp-state change callbacks. Each surface's ColorOverride fires
  // when wisp goes from un-controlling → controlling or vice versa
  // (edge-triggered inside maybeNotifyWispStateChange). The Flutter
  // app subscribes to CHAR_WISP_STATUS so a notify lands the moment
  // a surface transitions; the indicator widget pops on / off without
  // having to poll.
  baseColorOverride.setOnWispStateChangeCallback(
      []() { ble_control::notifyWispStatus(); });
  shadeColorOverride.setOnWispStateChangeCallback(
      []() { ble_control::notifyWispStatus(); });
  // Provider that the CHAR_WISP_STATUS read merges into the JSON. Lives
  // here so the ColorOverride globals stay out of the network layer.
  lamp::nearbyLamps.setLampWispStateProvider([]() {
    lamp::NearbyLamps::LampWispState ws;
    ws.controllingBase  = baseColorOverride.isWispActive();
    ws.controllingShade = shadeColorOverride.isWispActive();
    if (baseColorOverride.hasLastWispColor()) {
      ws.baseWispColor = lamp::colorToHexString(
          baseColorOverride.lastWispColor());
    }
    if (shadeColorOverride.hasLastWispColor()) {
      ws.shadeWispColor = lamp::colorToHexString(
          shadeColorOverride.lastWispColor());
    }
#ifdef LAMP_DEBUG
    // Pair with [wisp_state] notify ... in ble_control.cpp::notifyWispStatus.
    // Same call site, different lens: this prints what the LIVE override
    // state-machine is saying right now; the notify log prints what made
    // it into the JSON. Catches a stale-cache-vs-current-state mismatch
    // (none expected — they share the snapshot) and any sub-100ms races
    // around test_expression complete + reassertHold().
    Serial.printf("[wisp_state] provider isWispActive base=%d shade=%d hasBaseC=%d hasShadeC=%d\n",
                  ws.controllingBase ? 1 : 0,
                  ws.controllingShade ? 1 : 0,
                  baseColorOverride.hasLastWispColor() ? 1 : 0,
                  shadeColorOverride.hasLastWispColor() ? 1 : 0);
#endif
    return ws;
  });
  // BrightnessOverride routes its change-driven callback into the
  // existing applyEffectiveBrightness path so master-brightness fades
  // share the same NeoPixel setBrightness entry point.
  brightnessOverride.setOnChangeCallback([]() { applyEffectiveBrightness(); });

  // Personality engine — owns crowd-aware dimming (Introvert only),
  // disposition-driven greeting tuning, the Salty side-eye envelope
  // (consumed by effectiveBrightness as applySaltyDim), and the
  // disposition-driven arrival / Fond-bleed-contribution / closest-pulse
  // cycles. Bleed contributions are drawn additively by
  // shadePersonalityBehavior — no ColorOverride wiring. Its tick runs
  // in loop() after the transient-override tick block; sampleAndSmooth
  // gates most work behind a 1 Hz cadence so the per-loop cost is
  // negligible.
  lamp::personalityEngine.begin(&config, &expressionManager, &showReceiver);
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
    // `disabledDuringWispOverride` tolerated from older payloads but
    // ignored — pure type-property now.
    if (keyStr == "type" || keyStr == "enabled" ||
        keyStr == "disabledDuringWispOverride" ||
        keyStr == "intervalMin" || keyStr == "intervalMax" ||
        keyStr == "target" || keyStr == "colors") {
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
      // Just trigger the expression. No configurator gating needed:
      // expressions draw AFTER the configurator in the behavior list, so
      // they compose on top of wisp paint naturally and yield (via
      // animationState=STOPPED) when their one-shot animation completes.
      // The pre-2026-06-12 `configurator.disabled` workaround is gone.
      expressionManager.triggerExpression(type.c_str(), target);
    }
  } else if (action == "test_expression_complete") {
    // Configurator was never disabled by test_expression (post-2026-06-12
    // reorder makes expressions overlay the configurator instead of
    // racing it). This handler still serves a purpose: the app may have
    // edited the saved colors during the test and wants the configurator
    // to snap to the new values + the lamp to re-assert any active wisp
    // paint so the new baseline doesn't briefly stomp it.
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
    // If the wisp was painting either surface before the expression
    // test, the configurator-color writes just above stomped the wisp's
    // target gradient with the lamp's saved colors. Re-assert the wisp
    // paint immediately so the surface returns to what it was showing
    // pre-test, rather than waiting up to ~10s for the wisp's next
    // backstop paint cycle. No-op when the override isn't in Holding.
    shadeColorOverride.reassertHold();
    baseColorOverride.reassertHold();
  }
#ifdef LAMP_DEBUG
  // Personality dev-injection hook: replaces nearbyLamps view inside
  // PersonalityEngine so a developer can simulate a crowd from one
  // physical lamp + the Flutter app's test-action button. Without this,
  // verifying the 50% crowd-dim floor would need 10 lamps in BLE range
  // simultaneously.
  //
  // Payload:
  //   {"a":"inject_nearby","peers":[
  //     {"name":"red","baseColor":"#FF0000FF","disposition":5},
  //     {"name":"blue","baseColor":"#0000FFFF","disposition":1}
  //   ]}
  // Pair with {"a":"clear_nearby"} to drop back to live data.
  else if (action == "inject_nearby") {
    std::vector<lamp::NearbyLamp> peers;
    JsonArray arr = doc["peers"];
    for (JsonVariant v : arr) {
      lamp::NearbyLamp p;
      p.name = String(v["name"] | "").c_str();
      if (p.name.empty()) continue;
      const String baseHex = String(v["baseColor"] | "#FFFFFFFF");
      p.baseColor = lamp::hexStringToColor(baseHex.c_str());
      p.lastRssi = v["rssi"].is<int>() ? static_cast<int8_t>(v["rssi"].as<int>()) : -50;
      const int disp = v["disposition"] | 3;
      if (disp >= 1 && disp <= 5 && !p.name.empty()) {
        config.setDisposition(p.name, static_cast<uint8_t>(disp));
      }
      peers.push_back(p);
    }
    Serial.printf("[personality] inject_nearby count=%u\n", (unsigned)peers.size());
    lamp::personalityEngine.setNearbyOverride(std::move(peers));
  } else if (action == "clear_nearby") {
    Serial.println("[personality] clear_nearby");
    lamp::personalityEngine.clearNearbyOverride();
  }
#endif
}

extern void lamp_register_panic_handler();

// Effective brightness, mirroring calculateEffectiveHomeMode() so the
// brightness value and the compositor's homeMode gate stay in lockstep.
// See calculateEffectiveHomeMode below for the rule.
static uint8_t effectiveBrightness();
static bool calculateEffectiveHomeMode();
static uint8_t effectiveBrightness() {
  const uint8_t raw = calculateEffectiveHomeMode() ? config.homeMode.brightness
                                                    : config.lamp.brightness;
  // PersonalityEngine multipliers: crowd-dim (Introvert + smoothed peer
  // weight) THEN Salty side-eye envelope (1.0 → 0.70 → 1.0 over fade-in /
  // hold / fade-out). Both are multipliers in (0, 1] so order is
  // mathematically commutative; chained crowd-first matches the comment
  // history ("dimmed by personality, then by transient overrides").
  // Applied BEFORE the transient brightnessOverride.effective() so paint /
  // wisp overrides continue to work normally over the dimmed baseline.
  const uint8_t afterCrowd = lamp::personalityEngine.applyCrowdDim(raw);
  const uint8_t afterSalty = lamp::personalityEngine.applySaltyDim(afterCrowd);
  // OTA progress pulse (Phase 5b'.5). Innermost multiplier — applied LAST
  // so it modulates whatever the configurator / home-mode / personality
  // dimming yielded. Returns 100 (no effect) when no OTA flow is active;
  // single-pulse (sender) or double-pulse (receiver) cadence during a
  // flow. Multiplier never reaches 0 — lamp stays visibly lit.
  const uint8_t pulseMul = shadeSocialBehavior.otaPulseMultiplier(millis());
  return static_cast<uint8_t>((static_cast<uint16_t>(afterSalty) * pulseMul) / 100);
}

static void applyEffectiveBrightness() {
  // Phase C: when a BrightnessOverride is active, its interpolated value
  // takes precedence over the user's baseline. When inactive,
  // effective(baseline) just returns baseline so this is a no-op tax.
  // The override's change-driven callback also funnels back through this
  // function (see initBehaviors); recursion is bounded because the
  // callback only fires when the rounded value moved AND the override
  // doesn't re-read its own callback inside that critical section.
  uint8_t baseline = effectiveBrightness();
  uint8_t level = brightnessOverride.effective(millis(), baseline);
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
  wifi::setHomeModeEnabledGetter([]() { return config.homeMode.enabled; });
  // Suspend WiFi background scans while OTA is in flight — the scan hops
  // the radio across 11+ channels for ~5s and silently drops ESP-NOW
  // unicast (both OFFER→lamp AND lamp→wisp ACCEPT/REQ) during that window.
  wifi::setOtaInProgressGetter([]() { return firmwareReceiver.isInProgress(); });

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
  // Phase F: install the OTA receiver BEFORE showReceiver.begin() so the
  // chunk fast-path is wired by the time MSG_FW_* frames start arriving.
  // FirmwareReceiver::begin captures myMac via showReceiver.getMyMac(),
  // which is populated after the EspNowLink::begin() call inside
  // showReceiver.begin() — so receiver-side init has to run AFTER
  // showReceiver.begin(). The setFirmwareReceiver() call can happen any
  // time before the first MSG_FW_OFFER arrives, but installing it right
  // here keeps the wiring co-located.
  showReceiver.setFirmwareReceiver(&firmwareReceiver);
  // Bring up ESP-NOW grid presence (HELLO + COLORS). Independent of home
  // WiFi — runs on whatever channel the radio is on. See lamp_protocol.hpp.
  showReceiver.begin(&config);
  // Wire the ESP-NOW transport adapter onto the FirmwareReceiver. The
  // mesh path (wisp → lamp MSG_FW_*) emits via this transport. The BLE
  // transport is late-bound by ble_control::setFirmwareReceiver() below
  // (which uses the static BleFirmwareTransport instance internal to
  // ble_control.cpp).
  static lamp::EspNowFirmwareTransport meshFwTransport(&showReceiver);
  firmwareReceiver.begin(&meshFwTransport);

  // Wire app-driven BLE OTA: registers FirmwareReceiver with ble_control so
  // CHAR_FW_CONTROL writes (OFFER/DONE) and CHAR_FW_CHUNK writes route into
  // the receiver, and the receiver can notify ACCEPT/REQ/RESULT on
  // CHAR_FW_CONTROL. The single-source mutex in FirmwareReceiver enforces
  // that only one transport (mesh OR BLE) drives an active OTA at a time.
  ble_control::setFirmwareReceiver(&firmwareReceiver);

  // Gossip OTA (Phase 5b'): the lamp can ALSO originate offers to peers
  // it meets via the social system. Wire the distributor through the same
  // EspNowFirmwareTransport (it emits MSG_FW_OFFER/CHUNK/DONE and listens
  // for ACCEPT/REQ/RESULT via show_receiver's new dispatch ladder).
  lamp::firmwareDistributor.begin(&meshFwTransport);
  showReceiver.setFirmwareDistributor(&lamp::firmwareDistributor);

  // Phase G: arm the post-OTA self-health timer. esp_ota_get_state_partition
  // returns the ota-image-state of the running partition. If we just
  // booted from a freshly-OTA'd image, state == ESP_OTA_IMG_PENDING_VERIFY
  // and the bootloader will auto-rollback on the next reset unless we
  // explicitly mark the partition valid. We give the lamp 30 seconds of
  // steady-state runtime before declaring it healthy — long enough to
  // surface any boot-time crashes that would justify a rollback.
  //
  // Without this code path, the SECOND OTA permanently fails (esp_ota_begin
  // returns ESP_ERR_OTA_ROLLBACK_INVALID_STATE). One-shot-OTA trap. The
  // reconciliation doc's ESP32-specifics section calls this out as a Phase F
  // inline requirement.
  {
    esp_ota_img_states_t state;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
      if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        g_pendingVerify = true;
        g_markValidDeadlineMs = millis() + 30000;
#ifdef LAMP_DEBUG
        Serial.println("[ota] booted in PENDING_VERIFY, 30s self-health check armed");
#endif
      }
    }
  }
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
    // Stamp a new micro-fade triple instead of writing the strip
    // directly. The loop-tick interpolation below smooths each ~60-100 ms
    // BLE write into a continuous transition, eliminating the visibly
    // stepped brightness drag the user used to see. See the comment on
    // s_userBrightnessSource above for the why.
    const uint32_t fadeNow = millis();
    const uint8_t  source = s_userBrightnessSeeded
                                ? computeUserBrightnessNow(fadeNow)
                                : level;
    s_userBrightnessSource     = source;
    s_userBrightnessTarget     = level;
    s_userBrightnessFadeStartMs = fadeNow;
    s_userBrightnessSeeded     = true;
    // Apply the initial sample on this drain cycle so the strip starts
    // moving immediately rather than waiting for the next compositor
    // tick.
    if (shadeStrip) shadeStrip->setBrightness(
        lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, source));
    if (baseStrip)  baseStrip->setBrightness(
        lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, source));
    // ARCHITECTURAL INVARIANT for section cache:
    //   `*SectionJsonCached()` represents the PERSISTED config snapshot
    //   (last NVS write or boot-load), NOT the live in-memory config.
    //   The app's ControlNotifier compares its local state against the
    //   section JSON it reads via CHAR_LAMP_SECTION / CHAR_BASE_SECTION /
    //   etc. to know what's been "saved." If we invalidate the section
    //   cache here on a live-preview write, the app re-reads the section
    //   right after a save-and-reconnect (within its 5s reconnect window
    //   the brightness writer's debounced live write may also fire) and
    //   gets the LIVE value, not the persisted one. The save's
    //   `_original = fresh` then captures the live value as the
    //   "saved snapshot," which breaks isDirty comparisons forever after
    //   (until the next save fully reloads, or the lamp reboots).
    //
    //   Therefore: live-preview characteristic drains UPDATE the
    //   in-memory config (needed for effectiveBrightness, render math,
    //   etc.) but DO NOT invalidate the section cache. Only the
    //   settingsBlob persist path (which writes NVS) calls
    //   invalidateAllSections() — that's the canonical "now there's a
    //   new persisted snapshot" event.
    //
    //   See also the parallel drains for shadeColors, baseColors, and
    //   knockout below — same invariant, same comment by reference.
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
    shadeConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    baseConfiguratorBehavior.lastWebSocketUpdateTimeMs = millis();
    // Live preview only — does NOT invalidate the shade section cache.
    // See the architectural invariant comment at the brightness drain.
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
    // Live preview only — does NOT invalidate the base section cache.
    // See the architectural invariant comment at the brightness drain.
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
      // Live per-pixel knockout — does NOT invalidate the base section
      // cache. See the architectural invariant comment at the brightness
      // drain. Knockout map is persisted via the settingsBlob path on
      // the next save (knockoutPixels is included in base section JSON).
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
          // Log the in-memory config snapshot post-merge — this is what
          // the next boot reads back from NVS, so the next BLE section
          // read should match these values. Diagnostic for "save+reload
          // didn't actually persist" reports.
          Serial.printf("[loop] settingsBlob: persisted %u bytes, lamp.brightness=%u name='%s' socialMode=%u, fading for reboot\n",
                        (unsigned)written,
                        (unsigned)config.lamp.brightness,
                        config.lamp.name.c_str(),
                        (unsigned)config.lamp.socialMode);
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

  // Phase C transient-override drains. Each block drains its typed slot
  // and dispatches to the matching override instance based on the
  // `surface` byte. Surface::Any maps to "both surfaces" for the color
  // family (used by Wisp's whole-lamp recolor) and to "the brightness
  // override" for the brightness family (single global instance in v1).
  {
    lamp::PendingOverrideColors cmd;
    if (pendingOverrideColors.drain(pendingMux, cmd)) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] drain overrideColors surface=0x%02X n=%u fadeMs=%u\n",
                    (unsigned)cmd.surface, (unsigned)cmd.numColors,
                    (unsigned)cmd.fadeDurationMs);
#endif
      // Pin the wisp identity BEFORE the apply() calls. apply() can
      // edge-trigger the wisp-active transition callback, which calls
      // notifyWispStatus → reads getWispStatusReadJson. If the cache is
      // still "{}" at that moment, the notify carries an empty payload
      // and the app shows "No wisp detected" until the next wisp hello
      // (≤30s) populates the cache. Setting mac/present here first
      // makes the very first paint-triggered notify carry the truth.
      //
      // Without this fix: on a freshly-joined lamp that hears
      // OVERRIDE_COLORS (unicast paint, no relay) but hasn't yet heard
      // a WISP_HELLO (gossip-broadcast, 30s heartbeat), the app sees
      // "No wisp detected" even though the lamp is actively wisp-
      // painted — the "bytes=0 puzzle" observed in the field.
      if (cmd.sourceKind == lamp_protocol::OverrideSource::Wisp) {
        lamp::nearbyLamps.cacheWispMacFromPaint(cmd.sourceMac);
      }
      // Surface routing.
      // - Base: colors → base only.
      // - Shade: colors → shade only.
      // - Any: same colors → both base + shade (legacy whole-lamp recolor).
      // - BaseAndShade: colors[0] → base, colors[1] → shade (paired wisp
      //   paint, halves ESP-NOW traffic vs the prior two-frame design).
      //   Must have numColors >= 2; if only 1 color arrived, treat it as
      //   the base colour and skip shade (defensive, shouldn't happen).
      if (cmd.surface == lamp_protocol::OverrideSurface::BaseAndShade) {
        baseColorOverride.apply(cmd.sourceMac, cmd.sourceKind,
                                &cmd.colors[0], /*numColors=*/1,
                                cmd.fadeDurationMs);
        if (cmd.numColors >= 2) {
          shadeColorOverride.apply(cmd.sourceMac, cmd.sourceKind,
                                   &cmd.colors[1], /*numColors=*/1,
                                   cmd.fadeDurationMs);
        }
      } else {
        if (cmd.surface == lamp_protocol::OverrideSurface::Base ||
            cmd.surface == lamp_protocol::OverrideSurface::Any) {
          baseColorOverride.apply(cmd.sourceMac, cmd.sourceKind,
                                  cmd.colors, cmd.numColors,
                                  cmd.fadeDurationMs);
        }
        if (cmd.surface == lamp_protocol::OverrideSurface::Shade ||
            cmd.surface == lamp_protocol::OverrideSurface::Any) {
          shadeColorOverride.apply(cmd.sourceMac, cmd.sourceKind,
                                   cmd.colors, cmd.numColors,
                                   cmd.fadeDurationMs);
        }
      }
      // Wisp paint ships Base + Shade 10 ms apart per peer but both land
      // in a single-slot mailbox — newest-writer-wins. When Core 1 lags
      // past 10 ms (BLE scan + render contention is enough), the Shade
      // frame drops the Base frame on the floor and Base's watchdog
      // expires after 60 s. Treat the surviving frame as proof of mesh
      // liveness on both surfaces. PeerSwap ships single-surface frames,
      // so guard on Wisp only.
      if (cmd.sourceKind == lamp_protocol::OverrideSource::Wisp) {
        const uint32_t now = millis();
        baseColorOverride.touchApply(now);
        shadeColorOverride.touchApply(now);
        // Defensive notify: covers the case where apply()'s edge
        // detector didn't fire (e.g. we were already wisp-active on
        // both surfaces, but the wisp's mac changed). Idempotent on
        // the wire — fbp passes the bytes through and the app's
        // wispStatus subscription re-reads them.
        ble_control::notifyWispStatus();
      }
    }
  }
  {
    lamp::PendingRestoreColors cmd;
    if (pendingRestoreColors.drain(pendingMux, cmd)) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] drain restoreColors surface=0x%02X fadeMs=%u\n",
                    (unsigned)cmd.surface, (unsigned)cmd.fadeDurationMs);
#endif
      // BaseAndShade restores both surfaces in one frame (mirrors paint).
      if (cmd.surface == lamp_protocol::OverrideSurface::Base ||
          cmd.surface == lamp_protocol::OverrideSurface::Any ||
          cmd.surface == lamp_protocol::OverrideSurface::BaseAndShade) {
        baseColorOverride.restore(cmd.sourceMac, cmd.sourceKind,
                                  cmd.fadeDurationMs);
      }
      if (cmd.surface == lamp_protocol::OverrideSurface::Shade ||
          cmd.surface == lamp_protocol::OverrideSurface::Any ||
          cmd.surface == lamp_protocol::OverrideSurface::BaseAndShade) {
        shadeColorOverride.restore(cmd.sourceMac, cmd.sourceKind,
                                   cmd.fadeDurationMs);
      }
    }
  }
  {
    lamp::PendingOverrideBrightness cmd;
    if (pendingOverrideBrightness.drain(pendingMux, cmd)) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] drain overrideBrightness b=%u fadeMs=%u\n",
                    (unsigned)cmd.brightness, (unsigned)cmd.fadeDurationMs);
#endif
      brightnessOverride.apply(cmd.sourceMac, cmd.sourceKind, cmd.surface,
                               cmd.brightness, cmd.fadeDurationMs);
    }
  }
  {
    lamp::PendingRestoreBrightness cmd;
    if (pendingRestoreBrightness.drain(pendingMux, cmd)) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] drain restoreBrightness fadeMs=%u\n",
                    (unsigned)cmd.fadeDurationMs);
#endif
      brightnessOverride.restore(cmd.sourceMac, cmd.sourceKind,
                                 cmd.fadeDurationMs);
    }
  }
  {
    lamp::PendingWispHello cmd;
    if (pendingWispHello.drain(pendingMux, cmd)) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] drain wispHello flags=0x%02X v=0x%08X\n",
                    (unsigned)cmd.flags, (unsigned)cmd.wispVersion);
#endif
      lamp::nearbyLamps.cacheWispHello(cmd.sourceMac, cmd.wispVersion, cmd.flags,
                                       cmd.paletteIdPrefix, cmd.carriedFwChannel,
                                       cmd.carriedFwVersion);
    }
  }
  {
    // Wisp manualPalette drain. ShowReceiver's recv branch parsed +
    // deduped + posted; here on Core 1 we update the NearbyLamps cache
    // and push a BLE notify so subscribed apps see the new palette
    // without round-tripping a read of CHAR_WISP_STATUS.
    lamp::PendingWispPalette cmd;
    if (pendingWispPalette.drain(pendingMux, cmd)) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] drain wispPalette count=%u src=%02X:%02X:%02X:%02X:%02X:%02X\n",
                    (unsigned)cmd.count,
                    cmd.sourceMac[0], cmd.sourceMac[1], cmd.sourceMac[2],
                    cmd.sourceMac[3], cmd.sourceMac[4], cmd.sourceMac[5]);
#endif
      lamp::nearbyLamps.cacheWispPalette(cmd.sourceMac, cmd.rgb, cmd.count);
      ble_control::notifyWispStatus();
    }
  }
  // Phase D wispOp drain — app wrote a wispOp via CHAR_WISP_OP; we
  // broadcast it as MSG_CONTROL_OP so the wisp(s) on the mesh pick it
  // up. NEVER applied locally — wispOp is wisp-only (lamps don't have a
  // zone state). ShowReceiver::sendControlOp records its own seq into
  // controlOpDedup_ before the broadcast, so the reflected copy we get
  // back from the gossip rebroadcast doesn't try to apply locally.
  if (pendingWispOpJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint16_t len = pendingWispOpJson.drain(pendingMux, buf);
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain wispOp len=%u\n", (unsigned)len);
#endif
    if (len > 0) {
      // Removed the !isOtaInProgress() gate on 2026-06-13 after a sub-agent
      // audit confirmed it silently dropped user-driven wispOps (e.g.
      // `setSource off`) whenever a background gossip-OTA was in flight,
      // and the wisp never received the broadcast. Config ops are low-rate
      // and small; a few extra mesh bytes during OTA is acceptable in
      // exchange for the ops actually reaching the wisp every time the
      // user taps Save. The applyRemoteOpRouted() quiesce stays in place
      // because that forward path is gossip-relay (much higher volume).
      static const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      showReceiver.sendControlOp(kBroadcastMac,
                                 reinterpret_cast<const uint8_t*>(buf),
                                 len);
    }
  }
  // Phase D wispStatus drain — applyRemoteOpLocal saw a wispStatus
  // payload and posted it here with the wisp's source MAC. Cache into
  // NearbyLamps and push a BLE notification so any app subscribed to
  // CHAR_WISP_STATUS picks up the change without round-tripping a read.
  if (pendingWispStatusJson.valid) {
    char buf[MAX_PENDING_OP_JSON + 1];
    uint8_t srcMac[6];
    uint16_t len = pendingWispStatusJson.drain(pendingMux, buf, srcMac);
#ifdef LAMP_DEBUG
    Serial.printf("[loop] drain wispStatus len=%u src=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  (unsigned)len, srcMac[0], srcMac[1], srcMac[2],
                  srcMac[3], srcMac[4], srcMac[5]);
#endif
    if (len > 0) {
      lamp::nearbyLamps.cacheWispStatus(srcMac, buf, len);
      ble_control::notifyWispStatus();
    }
  }
  // MSG_EVENT drain. The recv side already resolved our delayMs from the
  // stagger entries list and copied the payload bytes into the slot;
  // tryHandleExpressionEvent does the (cheap-peek → RecentCascade dedup →
  // full parse → trigger) dance on Core 1. No local-config consult —
  // cascade is sender-authoritative.
  {
    lamp::PendingEvent cmd;
    if (pendingEvent.drain(pendingMux, cmd)) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] drain event src=%02X:%02X:%02X:%02X:%02X:%02X "
                    "delayMs=%u payloadLen=%u\n",
                    cmd.sourceMac[0], cmd.sourceMac[1], cmd.sourceMac[2],
                    cmd.sourceMac[3], cmd.sourceMac[4], cmd.sourceMac[5],
                    (unsigned)cmd.delayMs, (unsigned)cmd.payloadLen);
#endif
      expressionManager.tryHandleExpressionEvent(cmd.sourceMac, cmd.delayMs,
                                                 cmd.payload, cmd.payloadLen);
    }
  }
  // Phase F: MSG_FW_OFFER / MSG_FW_DONE drain. ShowReceiver's WiFi recv
  // path populates the slot; FirmwareReceiver does the heavy work on
  // Core 1 (esp_ota_begin, signature verify, set_boot_partition). The
  // chunk fast-path bypasses this slot — see show_receiver.cpp MSG_FW_
  // CHUNK branch.
  {
    lamp::PendingFirmwareControl cmd;
    if (pendingFirmwareControl.drain(pendingMux, cmd)) {
#ifdef LAMP_DEBUG
      Serial.printf("[loop] drain fwControl msgType=0x%02X seq=%u\n",
                    (unsigned)cmd.msgType, (unsigned)cmd.seq);
#endif
      firmwareReceiver.handleControlOnLoop(cmd);
    }
  }

  // Phase C: drive the override state machines. tick() is cheap when Idle
  // (single load + branch) so call unconditionally. Brightness tick uses
  // the same baseline as applyEffectiveBrightness so the change-driven
  // callback's view stays consistent across the fade window.
  {
    const uint32_t now = millis();
    baseColorOverride.tick(now);
    shadeColorOverride.tick(now);
    brightnessOverride.tick(now, effectiveBrightness());

    // User brightness micro-fade tick. Only runs when no transient
    // brightness override is active — applyEffectiveBrightness's
    // change-callback path owns the strip while the override is
    // animating (wisp / personality). Without this gate the two
    // would race on every frame.
    if (!brightnessOverride.isActive() && s_userBrightnessSeeded &&
        s_userBrightnessSource != s_userBrightnessTarget) {
      const uint8_t level = computeUserBrightnessNow(now);
      if (shadeStrip)
        shadeStrip->setBrightness(
            lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
      if (baseStrip)
        baseStrip->setBrightness(
            lamp::calculateBrightnessLevel(LAMP_MAX_BRIGHTNESS, level));
      // Mark the fade complete so subsequent frames are no-ops once
      // the window has elapsed.
      if (level == s_userBrightnessTarget) {
        s_userBrightnessSource = s_userBrightnessTarget;
      }
    }

    // Personality engine — runs after the transient-override block so it
    // reads a consistent post-fade view this tick. Cheap when nothing's
    // changed (1 Hz internal cadence). consumePendingApply() trips the
    // existing applyEffectiveBrightness pump when the crowd-dim factor
    // crosses the deadband, or when SocialMode flips out of Introvert.
    lamp::personalityEngine.tick(now);
    if (lamp::personalityEngine.consumePendingApply()) {
      pendingApplyEffectiveBrightness = true;
    }
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
  // Phase F: drive OTA stall watchdog + REQ generation + 60s hard cap.
  // Cheap when state == Idle (single switch + return).
  firmwareReceiver.tick(millis());
  lamp::firmwareDistributor.tick(millis());
  // Phase G: post-OTA self-health check. After 30 seconds of steady-state
  // loop iteration, if BLE is up and the loop has been iterating (we're
  // inside it now so trivially true), call mark_app_valid so the next OTA
  // can succeed. If the lamp crashes before the deadline, the bootloader
  // auto-rollbacks on the next reset.
  if (g_pendingVerify && static_cast<int32_t>(millis() - g_markValidDeadlineMs) >= 0) {
    if (ble_control::isRunning()) {
      const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
      if (err == ESP_OK) {
#ifdef LAMP_DEBUG
        Serial.println("[ota] new partition marked valid (post-OTA boot)");
#endif
      } else {
#ifdef LAMP_DEBUG
        Serial.printf("[ota] mark_app_valid failed: 0x%X\n", (unsigned)err);
#endif
      }
      g_pendingVerify = false;  // one-shot — even on failure don't retry
    } else {
      // BLE isn't running; defer another tick. If BLE never comes up,
      // the lamp's broken anyway and rollback is the right move.
      g_markValidDeadlineMs = millis() + 5000;  // re-check in 5s
    }
  }
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
