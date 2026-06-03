# Social tab

## Context

Today the Info tab contains both the Lamplit branding block AND two side-by-side columns: "Nearby" (live BLE scan) and "Seen recently" (persisted seen list). The user wants:

- Remove both columns from Info. Info becomes branding-only.
- New Social tab in the bottom nav.
- Top of Social: a personality selector — Introvert / Ambivert / Extrovert (horizontal segmented, default Ambivert), styled like the base/shade/both selector in advanced LED setup.
- Personality tunes how often the lamp greets:
  - **Extrovert:** always greets (minimal cooldown).
  - **Ambivert (default):** greets fairly often; tires after a burst of greetings and needs to recharge; won't re-greet the same lamp within a window.
  - **Introvert:** greets, then enters a random recharge window before the next greeting; long re-greet window per peer.
- Below the selector: a list of every lamp this lamp has seen, with last-seen timestamp and a 5-position disposition slider (hostile / wary / neutral / friendly / best-friend). Default neutral. Disposition is **stored only** for now — no behavioral effect yet.

## Architecture

Three subsystems, modest scope each:

### A. Firmware: `socialMode` in `LampSettings`

Add an enum `SocialMode { Introvert = 0, Ambivert = 1, Extrovert = 2 }` to `config_types.hpp`. Add `SocialMode socialMode = SocialMode::Ambivert;` to `LampSettings`. Round-trip in `config.cpp` load/save and in `Config::asLampJson` / `asJsonDocument`. Mirrors the existing `advancedEnabled` pattern exactly — no new BLE characteristic needed (rides settings_blob + CHAR_LAMP_SECTION).

### B. Firmware: `SocialBehavior` tuned by mode

`software/lamp-os/src/lamps/behaviors/social.cpp/.hpp` currently uses a single hard-coded `LAMP_TIME_BETWEEN_ACKNOWLEDGEMENT_MS = 30000` and a per-peer `acknowledged` flag in the transient `NearbyLamps` store (peers get pruned after 120s, then `acknowledged` is gone with them).

Replace with mode-driven timing + a small in-memory greeting log:

```cpp
class SocialBehavior {
  // ...
  // Per-peer last-greeted-at, keyed by name. Persists across NearbyLamp
  // prune cycles so re-greet windows work. Bounded to ~32 entries (LRU
  // by lastGreetedAtMs); name is the stable identity.
  std::map<std::string, uint32_t> lastGreetedAtMs_;
  
  // Sliding window of recent greeting timestamps for ambivert fatigue.
  // Drops entries older than 5 minutes; if size >= 5, we're "tired."
  std::vector<uint32_t> recentGreetMs_;
  uint32_t tiredUntilMs_ = 0;
};
```

`control()` logic:

```cpp
SocialMode mode = config->lamp.socialMode;
uint32_t now = millis();

// Per-mode base cooldown
uint32_t cooldownMs = (mode == Extrovert) ? 1000
                    : (mode == Introvert) ? 60000
                    :                       30000;

if (now < nextAcknowledgeTimeMs) return;

// Ambivert fatigue
if (mode == Ambivert && now < tiredUntilMs_) return;

// Find a peer to greet
for (auto& peer : nearbyLamps.getReachableViaBle(...)) {
  // Per-mode re-greet window — even if peer is "new" (not in
  // NearbyLamp's acknowledged flag), check our persistent log.
  uint32_t regreetWindowMs = (mode == Extrovert) ? 0
                           : (mode == Introvert) ? 600000   // 10 min
                           :                       300000;  // 5 min (ambi)
  auto it = lastGreetedAtMs_.find(peer.name);
  if (it != lastGreetedAtMs_.end() && now - it->second < regreetWindowMs) continue;
  
  // Greet
  triggerGreetingAnimation();
  lastGreetedAtMs_[peer.name] = now;
  recentGreetMs_.push_back(now);
  
  // Mode-specific post-greeting cooldown
  if (mode == Introvert) {
    // Random recharge 30-120s on top of base cooldown
    uint32_t recharge = 30000 + (esp_random() % 90000);
    nextAcknowledgeTimeMs = now + cooldownMs + recharge;
  } else {
    nextAcknowledgeTimeMs = now + cooldownMs;
  }
  
  // Ambivert fatigue: 5 greetings in last 5 min → tired for 60s
  if (mode == Ambivert) {
    while (!recentGreetMs_.empty() && now - recentGreetMs_.front() > 300000) {
      recentGreetMs_.erase(recentGreetMs_.begin());
    }
    if (recentGreetMs_.size() >= 5) {
      tiredUntilMs_ = now + 60000;
      recentGreetMs_.clear();
    }
  }
  
  break;  // one greeting per control() pass
}
```

Edge case: bound `lastGreetedAtMs_` to ~32 entries. If it grows past, evict the oldest. (Same scale as NearbyLamps.MAX_NEARBY.)

### C. Firmware: per-peer disposition persistence + BLE characteristic

Disposition lives in a **separate NVS key** (not in the main `cfg` blob) — keeps the per-section BLE reads small and avoids MTU pressure as the friend list grows.

- New file `software/lamp-os/src/config/dispositions.hpp/cpp` (or inline on Config — small).
- NVS namespace `lamp`, key `dispositions`. Stored as JSON object `{ "lampName1": 3, "lampName2": 5, ... }`. uint8_t 1-5.
- `Config::getDisposition(name) → uint8_t` (default 3 = neutral).
- `Config::setDisposition(name, value)` — bumps value into in-memory map, writes the full map back to NVS. Bounded ~100 entries (~3 KB JSON).
- New BLE characteristic `CHAR_SOCIAL_DISPOSITIONS` UUID `5f64f4e6-...` — read returns full JSON map, write replaces the full map. Auth-gated. Defined in `ble_control.hpp/cpp`.

Disposition has **zero behavioral effect** in this slice. We just persist it. Behavior tuning per disposition comes later (per user: "we will use this later").

### D. App: Social tab + remove from Info

- **Remove** the Nearby and Seen columns from `info_screen.dart` (lines 139-155). Info becomes just the wordmark + tagline + Lamplit blurb. (Existing tap-5-times unlock gesture stays — it's on the wordmark, which stays.)
- **Add** `LampTab.social` to the enum in `lamp_shell.dart:26`. Add NavigationDestination (icon: `Icons.handshake_outlined`) and a body case that renders `SocialScreen(lampId: widget.lampId)`. Place it between Setup and Info in the bar.
- **New file** `lib/features/social/presentation/social_screen.dart`:
  - Top: `SegmentedButton<SocialMode>` with three segments (Introvert / Ambivert / Extrovert). Backed by a new `ControlState.lamp.socialMode` field — riderd via the existing `setLampSocialMode` notifier method.
  - Middle: section label "Lamps you've met".
  - List: merge `nearbyLampsNotifierProvider` and `seenLampsNotifierProvider` outputs into one unified list (nearby lamps showing "now" or "X seconds ago", seen-but-not-nearby showing "X minutes/hours ago"). For each lamp: name, last-seen relative time, disposition slider.
- **Disposition slider widget:** small `Slider` with `divisions: 4`, range 1..5, labels "hostile … neutral … best friend". Value persisted via the new BLE characteristic + a dedicated `dispositionsNotifier` Riverpod provider that reads on connect and writes on change (debounced 500ms).

### E. App: `socialMode` plumbing

- Extend `LampSection.fromJson` and ControlState to carry `socialMode` (int 0-2 on wire, enum in Dart). Default to Ambivert.
- Add `setLampSocialMode(SocialMode)` to ControlNotifier — same pattern as `setLampAdvancedEnabled`: updates in-memory state, included in the dirty-detect for `save()`. (Save reboots the lamp, but the user expects that — same as any other Setup change.)

Actually — better UX: socialMode is a fire-and-forget single-byte change. We could write it directly via settings_blob immediately (with reboot), OR include it in the next batched Save. For consistency with how brightness/etc. work in the rest of Setup, **include in batched Save**. The Save button already exists and gates on `isDirty`.

### F. App: dispositions Riverpod provider

`lib/features/social/application/dispositions_notifier.dart`:

```dart
@Riverpod(keepAlive: false, name: 'dispositionsProvider')
class Dispositions extends _$Dispositions {
  Map<String, int> _staged = {};
  Timer? _flushTimer;
  
  @override
  Future<Map<String, int>> build(String lampId) async {
    // Read CHAR_SOCIAL_DISPOSITIONS, parse JSON, return map.
  }
  
  int get(String name) => state.value?[name] ?? 3;
  
  void set(String name, int value) {
    // Optimistic update + debounced write back.
  }
}
```

## Files to create / modify

**Firmware:**
- `software/lamp-os/src/config/config_types.hpp` — `SocialMode` enum, field on `LampSettings`.
- `software/lamp-os/src/config/config.cpp` — load/save `socialMode`; new disposition map load/save + helpers.
- `software/lamp-os/src/lamps/behaviors/social.hpp/.cpp` — mode-aware control loop, per-peer last-greeted map, fatigue window.
- `software/lamp-os/src/components/network/ble_control.hpp/.cpp` — new `CHAR_SOCIAL_DISPOSITIONS` characteristic (read + write, GCM-encrypted), serializer/deserializer.

**App:**
- `software/lamp-app-flutter/lib/features/control/domain/sections.dart` — `LampSection.socialMode` field.
- `software/lamp-app-flutter/lib/features/control/application/control_notifier.dart` — `setLampSocialMode(...)`; include in `save()` dirty path.
- `software/lamp-app-flutter/lib/features/lamp_shell/presentation/info_screen.dart` — remove Nearby/Seen columns.
- `software/lamp-app-flutter/lib/features/lamp_shell/presentation/lamp_shell.dart` — add `social` tab.
- `software/lamp-app-flutter/lib/features/social/` (new directory):
  - `presentation/social_screen.dart` — main tab content.
  - `application/dispositions_notifier.dart` — Riverpod provider for disposition map.
  - `domain/social_mode.dart` — enum + helpers.

## Verification

- `pio test -e native` — 11/11 pass.
- `pio run -e upesy_wroom` — clean build.
- `dart analyze` — clean.
- `flutter test` — all pass (may need to update setup_screen_test if Info changes break it; unlikely).
- Hardware:
  - With two lamps: set lamp A to Extrovert, lamp B to Introvert. Verify A greets immediately, B has long cooldowns.
  - Set lamp A to Ambivert. Toggle lamp B's BLE presence rapidly (5+ times in 5 min). Verify A goes "tired" after the 5th greeting and stops greeting for ~60s, then resumes.
  - Move disposition slider for a lamp. Disconnect/reconnect — disposition persists.
  - Open Info tab — only branding + tagline visible, no lamp lists.
  - Open Social tab — personality selector + lamp list with sliders visible.

## Out of scope (deferred)

- **Disposition-driven behavior** (e.g. friendly lamps cascade more, hostile ones get muted). Per user: "we will use this later."
- **Names for the disposition slider stops.** Labels can be plain "1 ... 5" with a single end-label legend ("hostile" / "best friend") if names are bikeshed-y. We can iterate.
- **Bringing dispositions cross-lamp** — i.e. lamp A's dispositions don't sync to lamp B. Each lamp has its own view. That matches the per-lamp config model.
- **Migration:** existing lamps come up with empty disposition map and `socialMode = Ambivert` (default). No migration needed.

## Open items the user can adjust later

- Exact cooldown numbers (30s/60s/1s) — guesses based on the user's prose, easy to tune.
- Ambivert fatigue threshold (5 greetings / 5 minutes / 60s tired) — same.
- Introvert recharge range (30-120s) — same.
