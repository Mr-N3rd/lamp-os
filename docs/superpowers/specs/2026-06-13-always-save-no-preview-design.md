# Always Save, No Preview — Design Spec

**Date:** 2026-06-13
**Status:** Approved for implementation planning
**Branch target:** new branch off `beta` post-stability-push
**Phasing:** Phase A (firmware-side, fleet-OTA-deliverable) → Phase B (app-only)

## Context

The lamp control surface today blends three save strategies that the user doesn't see consistently:

1. **Live-preview chars** (CHAR_BRIGHTNESS, CHAR_BASE_COLORS, CHAR_SHADE_COLORS, CHAR_BASE_KNOCKOUT) — update in-memory config + render state, never write NVS. Per the architectural invariant documented at `software/lamp-os/src/lamps/standard_lamp.cpp:1272-1280`.
2. **settings_blob** (CHAR_SETTINGS_BLOB) — the canonical "save NVS" path. Always reboots the lamp to make the on-disk merged JSON take effect.
3. **Per-feature persistent chars** — CHAR_EXPRESSION_OP, CHAR_SOCIAL_DISPOSITIONS, CHAR_WISP_OP each have their own NVS keyspace and their own write semantics.

The audit on 2026-06-13 (see today's session transcript) found ~15 panes where the user's edit either:
- Updates app state but fires no BLE write (rename, personality, home toggle, home SSID, advanced LED's Update button)
- Fires a live-preview write that previews on the lamp but reverts on power-cycle (brightness, base/shade colors, knockout, home-mode brightness)
- Updates `state` but never advances `_original`, leaving `isDirty` stuck true forever (expression upsert pre-fix)

The AppBar "Save changes" pill is the sole UI caller of `controlNotifier.save()` (`software/lamp-app-flutter/lib/features/lamp_shell/presentation/lamp_shell.dart:217-295`). Tapping it fades-out + reboots the lamp; many users don't realize it exists or what it does.

This spec replaces that triad with **a single mental model**: every user-visible edit either persists immediately or surfaces a clear failure.

## Goal

- Every editable surface in the lamp control app commits-on-edit.
- No "unsaved changes" indicator.
- No global Save button.
- Failed persists surface as a snackbar + state revert, not silent loss.
- Live preview during slider drags and color picks stays exactly as it is today.
- Reboot path stays *only* for changes that genuinely require it (Advanced LED strip reinit, factoryReset).

## Non-goals

- Mesh protocol version change (stays v0x03 — no cross-fleet break).
- New cryptography (settings_blob auth stays as today; CHAR_COMMIT reuses the same isAuthed gate).
- Multi-user / multi-app concurrent-edit arbitration (current "last write wins per field" is documented as acceptable; CHAR_EDIT_SESSION-style session tokens are deferred).
- Disposition map keyed by lamp name → MAC migration (pre-existing bug; documented for future, not in scope here).

## Architecture

Two write paths replace the live-preview-or-settings_blob duality. Both result in NVS persist. Neither reboots unless explicitly requested.

### Path 1 — Live-preview → CHAR_COMMIT (drag-style editing)

For UI flows where the user drags/scrubs a value and then "lifts" (slider onChangeEnd, editor Update tap):

- App writes the existing live-preview chars per drag tick. Lamp updates `config.<section>` in RAM + renders. **No NVS.**
- On the natural fence (slider release / editor Update), app writes a **new parameterless CHAR_COMMIT**. Firmware drains by calling `Config::persistConfig()` on whatever's currently in RAM.
- On Cancel, app writes the captured-at-open values back via the live-preview char so the lamp visually reverts. No commit.

### Path 2 — settings_blob (discrete actions + reboot-required)

For UI flows where a single action carries the new value (rename dialog Save, personality pill tap, home toggle, Advanced LED Update):

- App writes a partial-section JSON to settings_blob (e.g., `{"lamp": {"name": "foo"}}`).
- Firmware drain parses, dispatches to per-section `applyXxxLocal` helpers that update RAM in place, then calls `persistConfig()`.
- Reboot becomes opt-in via `{"reboot": true}` in the payload. Used for Advanced LED (strip reinit) and factoryReset.
- **Backward compat**: settings_blob's `reboot` default stays `true` so old apps that don't know about the flag keep working as today. New app explicitly sends `{"reboot": false}`.

## Phase A — Firmware

Phase A ships to the fleet via gossip OTA. App can stay unchanged on the bench during Phase A; the new char is dormant until Phase B starts using it.

### A.1 — Split user-source vs remote-source mutation paths (load-bearing prerequisite)

**Problem identified by the protocol audit**: today's `applyRemoteOpLocal` (in `standard_lamp.cpp` around line 487) calls `applyShadeColorsLocal`, `applyBaseColorsLocal`, the brightness drain logic, and `applyExpressionOpLocal` — all of which mutate `config.*` directly. Once CHAR_COMMIT serializes the current `config` snapshot, mesh-relayed transient values (cascade brightness pulses, peer-swap paints, social-greet cascades) become silently persistable.

**Fix**: each `applyXxxLocal` splits into two variants:
- `applyXxxToConfig(...)` — full mutation (config + render + cache invalidation). Called only from user-direct BLE drain paths and from settings_blob's per-section dispatch.
- `applyXxxToRender(...)` — render-only path (override/compositor surfaces, no `config.*` mutation). Called from `applyRemoteOpLocal` for mesh-relayed CONTROL_OPs.

Scope:
- `applyBrightnessLocal` → split (today the brightness drain inlines this; extract first).
- `applyShadeColorsLocal` / `applyBaseColorsLocal` → split.
- `applyExpressionOpLocal` → split. Today already eagerly persists via `persistConfig()` at `standard_lamp.cpp:~1374`; the remote-source variant must NOT trigger that persist.

**Acceptance**: after A.1, replaying mesh CONTROL_OPs against a freshly-booted lamp leaves `config.*` byte-identical to its NVS state. A native test asserts this via `test_apply_remote_no_config_mutation`.

### A.2 — New CHAR_COMMIT characteristic

UUID: pick a fresh one in the control service (mint via `python -c "import uuid; print(uuid.uuid4())"`).

Properties: `write` (with-response, **not** WRITE_NR — BLE disconnect during a commit must be a recoverable error, not a silent loss).

Payload: single-byte plaintext sentinel (e.g., `0x01`) when password is unset; AES-GCM ciphertext (29-byte minimum: `0x02 + 12B nonce + 16B tag`) when password is set. The callback IGNORES the payload — its semantic is "commit signal."

Callback shape: mirror `BrightnessCallback` in `software/lamp-os/src/components/network/ble_control.cpp:466-485`. Do NOT use WriteRouter — it rejects empty payloads and adds JSON parsing the commit doesn't need.

```cpp
// Sketch — exact shape per existing callback style
class CommitCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic*, NimBLEConnInfo& info) override {
    if (!isAuthed(info.getConnHandle())) return;
    pendingCommit = true;
  }
};
```

Post: `volatile bool pendingCommit` (single-bool naturally atomic on Xtensa; no portMUX needed).

### A.3 — CHAR_COMMIT drain (loop task)

Drain in `standard_lamp.cpp`'s loop drain block. Place AFTER the per-section live-preview drains (brightness, shadeColors, baseColors, knockout, expressionOp) so the persist sees their just-mutated state. Place BEFORE the settings_blob drain so a commit and a discrete blob write in the same tick don't fight.

Debounce: mirror the dispositions pattern at `software/lamp-os/src/config/config.cpp:~270`. Set `cfgDirty_` flag + `lastCommitMs_` timestamp on each `pendingCommit = true` arrival. Flush only when idle ≥ `kCommitFlushIdleMs` (initial: 1500 ms) OR on BLE disconnect.

On flush:
1. **Gate on `!firmwareReceiver.isInProgress()`**. A persist concurrent with OTA chunk-writes competes for the same NVS subsystem and can corrupt OTA. If OTA is in progress, defer the flush — re-check next tick.
2. Compute `currentSerialized = serializeJson(config.asJsonDocument())`.
3. Hash-dedup against `lastPersistedHash_`. If equal, skip the write — saves NVS wear on the "user dragged but ended at the same value" case.
4. `config.persistConfig(via="commit")` — see A.6 for path-tagged log.
5. On success: `config.invalidateAllSections()` + `ble_control::notifyStateChange()`.
6. On failure: `ble_control::notifyStateChange()` (so app can detect in-RAM vs NVS skew and re-read), log loud.

### A.4 — settings_blob drain rewrite

The existing drain at `standard_lamp.cpp:1385-1490` does: parse → merge into `fullDoc` → serialize → `prefs.putString("cfg", ...)` → unconditional reboot.

New shape:

1. **factoryReset short-circuit unchanged** at the top. Add a `Serial.printf` warning if `incomingDoc.size() > 1` (factoryReset co-shipped with other keys). App-side guard already ensures this can't happen; assert is forward-defense.
2. **OTA-in-progress interlock**: if `firmwareReceiver.isInProgress()`, log loud and discard the write. App will re-issue when OTA finishes.
3. Parse → for each top-level key in incoming blob (other than `reboot` and `factoryReset`):
   - Dispatch to user-source `applyXxxLocal` helper for that section. **Skip the `expressions` key** — see A.5.
4. `config.persistConfig(via="settings_blob")`.
5. `config.invalidateAllSections()` + `ble_control::notifyStateChange()`.
6. Reboot decision: `incomingDoc["reboot"] | true` — **default true** for backward compat. Set `lamp::fadeOutRebootRequested = true` if true.

### A.5 — `applyXxxLocal` helpers (user-source variants)

New / extracted helpers:

- **`applyLampLocal(JsonObject)`** — handles `name`, `brightness`, `advancedEnabled`, `socialMode`.
  - On name change: call `NimBLEDevice::setDeviceName(newName)` so the BLE advertised name updates without reboot. Mesh HELLO (`software/lamp-os/src/components/network/show_receiver.cpp:474`) already reads `config.lamp.name` live every 5 s, so peers see the new name on the next tick.
  - On brightness: call new helper `applyBrightnessImmediate(level, isHomeMode)` that updates the config field + invokes `applyEffectiveBrightness()`. **Does not** seed the micro-fade triple — that's a slider-live-preview-only concern.
- **`applyHomeModeLocal(JsonObject)`** — handles `ssid`, `password`, `enabled`, `brightness`.
- **`applyBaseAcLocal(int)`** — tiny, for the `base.ac` field that has no live-preview char.
- **`applyBaseKnockoutLocal(JsonArray)`** — extracted from inline knockout drain logic at `standard_lamp.cpp:~1325` so settings_blob's full-section path can reuse.

**Explicitly skipped**: `applyExpressionsLocal`. The per-entry CHAR_EXPRESSION_OP path is the canonical expression mutator and already eagerly persists. A wholesale `expressions[]` overwrite from settings_blob would silently drift `expressionManager` (the live runtime) from `config.expressions` (the persisted snapshot). Document in `docs/mesh-api.md` that settings_blob ignores `expressions` and mutators must use CHAR_EXPRESSION_OP.

**Section cache**: per-section `applyXxxLocal` helpers do NOT invalidate caches inline. The single canonical `invalidateAllSections()` happens at the persist site in A.3 / A.4. Easier to reason about; matches "persist + invalidate are atomic conceptually."

### A.6 — Path-tagged `persistConfig` log

Today's log line at `software/lamp-os/src/config/config.cpp:~294`: `[nvs] persistConfig wrote N bytes`. With three callers (CHAR_COMMIT, settings_blob, CHAR_EXPRESSION_OP) all hitting this function, the log is ambiguous when fleet-debugging "did my save take?"

Change signature: `bool Config::persistConfig(const char* via)`. Log: `[nvs] persistConfig via=commit wrote 4231 bytes` / `via=settings_blob` / `via=expressionOp`. Trivial change, big debuggability win.

### A.7 — Keep CHAR_HOMEMODEFOCUS routing

The existing routing at `software/lamp-os/src/components/network/ble_control.cpp:529-542` flips a flag that routes CHAR_BRIGHTNESS writes to `config.homeMode.brightness` vs `config.lamp.brightness`. With always-save, the home mode pane could write its own settings_blob with `homeMode.brightness` set, but the **live preview during a slider drag** still needs the routing — otherwise the slider in the home-mode pane would push to the wrong field. 14 lines, load-bearing, keep.

### A.8 — Native tests (Phase A)

All in `software/lamp-os/test/`. Native env, no hardware.

- `test_commit_drain` — CHAR_COMMIT signal triggers `persistConfig` after the debounce idle window. Verifies dirty-flag latching, hash-dedup skips identical commits, OTA interlock defers.
- `test_settings_blob_no_reboot` — `{"reboot": false}` writes NVS but leaves `fadeOutRebootRequested` false.
- `test_settings_blob_reboot_default` — blob without explicit `reboot` key reboots (backward compat).
- `test_settings_blob_factory_reset` — regression for the existing factoryReset path. Currently untested.
- `test_apply_remote_no_config_mutation` — replaying brightness/colors/expression remote ops leaves `config.*` byte-identical.
- `test_apply_brightness_immediate` — settings_blob brightness writes apply through `applyBrightnessImmediate` and skip the micro-fade triple.

Acceptance for Phase A: existing 272 native tests stay green + 6 new tests, total 278 green.

## Phase B — App

Phase B is app-only — `flutter build apk --debug` + `adb install -r` for a Pixel test. No firmware changes ship with Phase B. Risk is `adb install -r`-reversible.

### B.1 — `_mutate` helper

Wrap the prev-capture-and-revert pattern from `wisp_notifier.dart`'s setSource (today's setSource is the golden template):

```dart
Future<void> _mutate(
  ControlState Function(ControlState) transform,
  Future<void> Function() commit,
) async {
  final prev = state.value;
  if (prev == null) return;
  state = AsyncData(transform(prev));
  try {
    await commit();
  } catch (e) {
    state = AsyncData(prev);
    rethrow;
  }
}
```

~24 mutator call sites collapse from the explicit prev/restore dance to one `_mutate(...)` call. Per-call-site error handling stays — the caller catches the rethrow and shows the snackbar.

### B.2 — Per-pane mutator changes

| Pane | New mutator behavior |
|---|---|
| Brightness slider (lamp) | live-preview unchanged; on `onChangeEnd`, schedule 500 ms debounced `commit()` |
| Brightness slider (home mode) | same; routes via existing CHAR_HOMEMODEFOCUS during edit |
| Shade editor Update | live-preview unchanged; on Update tap → `commit()` + pop |
| Base editor Update | live-preview unchanged; on Update tap → if `base.ac` changed, `writeSettingsBlob({"base":{"ac":N}})` then `commit()` else just `commit()`; pop |
| Shade/Base editor Cancel | live-preview write with `_capturedColors` snapshot → pop. **No commit.** |
| Knockout pane | live-preview unchanged; on screen back, force-flush the debounced commit |
| Rename dialog Save | `writeSettingsBlob({"lamp":{"name":n}}, reboot: false)` |
| Personality pill tap | `writeSettingsBlob({"lamp":{"socialMode":mode}}, reboot: false)` |
| Home toggle | `writeSettingsBlob({"homeMode":{"enabled":b}}, reboot: false)` |
| Home SSID picker | `writeSettingsBlob({"homeMode":{"ssid":s,"password":p}}, reboot: false)` |
| Home Forget | `writeSettingsBlob({"homeMode":{"ssid":"","password":""}}, reboot: false)` |
| Advanced LED Update | `writeSettingsBlob({...}, reboot: true)` + post-reconnect verify (see B.4) |
| Factory Reset | `writeSettingsBlob({"factoryReset": true}, reboot: true)` (unchanged) |
| Expression Update / Delete | unchanged (already commits via CHAR_EXPRESSION_OP) |
| Wisp pane (all) | unchanged (wisp manages own NVS) |
| Per-peer disposition | unchanged (own NVS key + own debounce) |

### B.3 — GATT-discovery fallback (transition safety)

If the lamp doesn't expose CHAR_COMMIT (running pre-Phase-A firmware), the app must fall back to a settings_blob write — otherwise drag-style commits silently disappear.

Implementation:
- `BleClient` (or its caller) probes the control service's characteristic list at connect time and exposes `bool hasCommitChar`.
- The notifier exposes `commit({Section? section})` — section is metadata used by the fallback path only.
  - `hasCommitChar == true` → write CHAR_COMMIT (single-byte sentinel / encrypted). The `section` argument is unused on the wire — the firmware persists current RAM state regardless.
  - `hasCommitChar == false` → synthesize a partial blob from `state.value` for the named section and write `settingsBlob({"<section>": <currentValue>}, reboot: false)`. Each call site that fires `commit` knows which section it just edited, so the section parameter is local knowledge, not a wire parameter.

This keeps CHAR_COMMIT itself purely parameterless (which is its safety property — can't be wrong because it carries no data) while the app's fallback layer fills in the data dimension at the higher level.

Once the entire fleet has Phase A firmware, the fallback path can be deleted in a follow-up PR.

### B.4 — `{reboot: true}` post-reconnect verification

After `writeSettingsBlob(..., reboot: true)`:
1. App swallows `BleDisconnectedException` as expected (existing `setLampPassword` pattern).
2. After reconnect + reload, app diffs the section it shipped against what came back from `_readSections`.
3. On mismatch: snackbar "Save didn't take — retry?" with an action that retries the write.
4. On match: no UI surface (silent success).

This catches the firmware-side parse-failure footgun where settings_blob's JSON parse fails, persist skips, no reboot fires, but the BLE link stayed up briefly — the app would see a clean response but the change wasn't applied.

### B.5 — Debounce timer dispose + lifecycle flush

The new 500 ms brightness commit timer:
- Lives on `controlNotifier`.
- Registered in `ref.onDispose(() { _commitTimer?.cancel(); _flushPendingCommit(); })`. On notifier teardown, fire the pending commit so a user navigating away from a lamp doesn't lose their last slider position.
- Hook into `AppLifecycleState.paused`: flush trailing on app background. Mirrors the existing `_seenFlushTimer` + dispositions dispose patterns.

Brightness LIVE-PREVIEW writes (existing 60 ms WriteCoalescer) already debounce separately; the new commit timer chains AFTER the coalescer's trailing flush, not in parallel. Sequence at slider release:

```
onChangeEnd
  → WriteCoalescer.flush()    [completes within ~60ms — last live-preview write]
  → await
  → schedule commitTimer(500ms)
  → ... 500ms idle ...
  → commit()
```

### B.6 — Live-preview snap-back on Cancel

Color editor sheets (`shade_editor_sheet.dart`, `base_editor_sheet.dart`):
- On open: `_capturedColors = state.value.<shade|base>.colors`. (Same place captured today; semantics unchanged.)
- On Cancel: `await setShadeColors(_capturedColors)` (or `setBaseColors`) — fires live-preview char so lamp visually reverts. Pop. **No commit.**

**Critical**: on Update tap, the commit MUST fire (per app architecture audit). Don't pop without committing — that leaves the lamp painting an uncommitted color that snaps back to the captured value on next disconnect / section re-read.

### B.7 — Inventory cache immediate update on rename

Today's `setLampName` path runs `_inv.updateName(_deviceId, fresh.lamp.name)` AFTER `_awaitReconnectAndReload` returns the post-reboot fresh state (`control_notifier.dart:~420`).

New always-save world has no reload. Rename mutator must update the inventory cache **synchronously** as part of the optimistic state update:

```dart
Future<void> setLampName(String name) async {
  await _mutate(
    (s) => s.copyWith(lamp: s.lamp.copyWith(name: name)),
    () async {
      await _writeSettingsBlob({"lamp": {"name": name}}, reboot: false);
      // Inventory cache reflects the new name so LampChip AppBar title
      // updates without waiting for a reload that won't happen.
      await ref.read(inventoryNotifierProvider.notifier).updateName(_deviceId, name);
    },
  );
}
```

### B.8 — Nonce-reuse verification (security gate)

Before merging Phase B, verify that `LampCrypto.encryptOp` does NOT use a deterministic nonce derived solely from salt+password. Today's `save()` writes ONE settings_blob per save (no concurrency risk). New design fires multiple discrete settings_blob writes in close succession (rename + personality + home toggle within a session).

If nonce is `HKDF(salt+password+counter)` or includes a random component, safe — proceed.

If deterministic on salt+password, two writes with the same key+nonce is an AES-GCM nonce-reuse — confidentiality and authenticity both break. Required mitigation: serialize discrete settings_blob writes through a single-slot mutex on the notifier (similar pattern to how `_writeRouter` queues, but at the notifier layer).

Add a Flutter test asserting that two concurrent `writeSettingsBlob` calls don't reuse the same nonce.

### B.9 — Deletions

When all panes are wired to the new pattern:
- `controlNotifier.save()`
- `_isLampDirty`, `_isBaseDirty`, `_isShadeDirty`, `_isHomeDirty`, `_isExpressionsDirty`
- `_original` field + `_awaitReconnectAndReload`
- `isDirty` getter
- `lampSaveStatusProvider` + `lamp_save_status.dart` + `.g.dart`
- `_SaveAction` widget in `lamp_shell.dart` (+ the `actions: [...]` reference)
- "Saving changes…" branch in `connecting_view.dart` (revert to plain Connecting state)
- `inventory_notifier.dart`'s `_invalidatePerLampState` line that touches `lampSaveStatusProvider(id)` (just drop that line, keep `advancedSessionProvider(id)`)

### B.10 — Tests (Phase B)

Affected today (per app-architecture audit):
- `test/features/control/application/control_notifier_test.dart` — 8 tests across lines ~106-856 reference removed API. Rewrite as new pattern:
  - `save() merges local state into the full settings blob` → "discrete edit ships partial blob with only the touched section"
  - `save() does NOT ship knockout in the blob` → still valid; knockout commit shouldn't include the `base.knockout` map
  - `save() omits home password when sentinel` → reframe as "writeSettingsBlob omits home password when sentinel"
  - `isDirty` flip tests → replace with "discrete edit lands a CHAR_SETTINGS_BLOB write within Nms"
- `test/features/lamp_shell/lamp_shell_test.dart` — 3 widget tests at lines ~194-287. Delete — the Save pill is gone; "AppBar action absent on Info tab" becomes "AppBar has no actions on any tab."
- New tests modeled on `test/features/wisp/wisp_notifier_rollback_test.dart` (the golden template):
  - `setLampName commit failure reverts state and shows snackbar`
  - `commit timer is force-flushed on AppLifecycleState.paused`
  - `commit timer is force-flushed on notifier dispose`
  - `GATT-discovery fallback to settings_blob when CHAR_COMMIT absent`
  - `concurrent writeSettingsBlob calls do not reuse AES-GCM nonces` (per B.8)

Acceptance for Phase B: existing 340 Flutter tests minus deletions + ~6 new tests, total ~342 green.

## Bench test matrix (pre-fleet rollout)

Run on jacko + floral + Pixel after Phase A flashes, before Phase B install:

1. **Rename persists**: rename jacko via Phase A firmware + OLD app (uses settings_blob with default reboot=true). Power-cycle. Reconnect. Verify name persists.
2. **Old-app + new-firmware compat**: old app on Pixel, new firmware on jacko. Tap Save pill. Verify settings_blob still triggers reboot + persist.
3. **Phase B: brightness commit-after-debounce**: drag slider, release, wait 600 ms, power-cycle. Verify last brightness persists.
4. **Phase B: brightness commit-during-debounce loss window**: drag, release, power-cycle within 500 ms. Document loss (likely intentional — debounce expired before commit fired).
5. **Phase B: mid-edit BLE disconnect**: rename via dialog. Toggle Bluetooth off mid-write. Verify snackbar fires, local state reverts, no crash.
6. **Phase B: concurrent two-lamp edits**: rename jacko AND change floral shade colors back-to-back from one Pixel. Verify both lamps persist independently.
7. **Phase B: factory reset path**: still wipes NVS + reboots (`standard_lamp.cpp:~1398` short-circuit intact).
8. **Phase B: `{reboot:true}` post-reconnect verify**: write Advanced LED with px change. Verify reboot, reconnect, sections match shipped values.
9. **Phase B: knockout per-pixel persist**: edit knockout. Wait for debounce. Power-cycle. Verify map survives. (This is the highest-risk regression because knockout previously only persisted via global Save.)
10. **NVS wear smoke test**: drag swatch picker for 60 s on jacko. Count `[nvs] persistConfig via=commit wrote N bytes` log lines. Sanity-check it's <30, not >300.
11. **OTA-during-commit**: trigger gossip OTA on jacko. While OTA in progress, fire CHAR_COMMIT. Verify drain logs "OTA in progress, deferred."
12. **Mesh cascade contamination regression**: have floral fire a brightness cascade to jacko. Trigger CHAR_COMMIT on jacko within the cascade window. Power-cycle. Verify jacko's persisted brightness is the LOCAL value, not the cascade value.

## Deferred / documented

These are real issues surfaced by the audits but not in this spec's scope. Document in commit / mesh-api as appropriate:

- **Disposition map keyed by lamp name** (`software/lamp-os/src/config/config.hpp:~174` "KNOWN LIMITATION"): rename frictionless after this refactor means users hit the name-changed-disposition-orphaned bug more often. Fix in a separate spec; switch keying to MAC.
- **Multi-app concurrent-edit arbitration**: "last write wins per field" is the documented behavior. Multi-phone editing the same lamp simultaneously can produce mixed final state. Acceptable for current usage; CHAR_EDIT_SESSION-style session tokens deferred.
- **Hash-dedup edge case**: if a user drags through a transient that matches the last-persisted hash, the commit no-ops. UX: imperceptible. Document and move on.

## File map (critical files)

**Firmware (Phase A):**
- `software/lamp-os/src/lamps/standard_lamp.cpp:407-472` (applyExpressionOpLocal, applyXxxLocal split)
- `software/lamp-os/src/lamps/standard_lamp.cpp:487-545` (applyRemoteOpLocal — needs the To-Render variants)
- `software/lamp-os/src/lamps/standard_lamp.cpp:1219-1490` (full drain block; CHAR_COMMIT drain inserted here, settings_blob rewritten)
- `software/lamp-os/src/components/network/ble_control.cpp:178-470` (CharCallback shapes; new CommitCallback registered here)
- `software/lamp-os/src/components/network/ble_control.cpp:760-820` (SettingsBlobCallback — receives `reboot` flag now)
- `software/lamp-os/src/config/config.hpp:84-105` (Config::persistConfig signature change)
- `software/lamp-os/src/config/config.cpp:272-298` (persistConfig path-tagged log)
- `software/lamp-os/src/components/network/show_receiver.cpp:474` (mesh HELLO reads name live — confirm)
- `software/lamp-os/test/test_protocol_v2/` (new test files A.8)

**App (Phase B):**
- `software/lamp-app-flutter/lib/features/control/application/control_notifier.dart` (everything — new helpers, mutator rewrites, deletions)
- `software/lamp-app-flutter/lib/features/control/application/lamp_save_status.dart` (delete)
- `software/lamp-app-flutter/lib/features/control/presentation/widgets/connecting_view.dart` (Saving changes branch delete)
- `software/lamp-app-flutter/lib/features/control/presentation/widgets/brightness_card.dart` (onChangeEnd → schedule commit)
- `software/lamp-app-flutter/lib/features/control/presentation/widgets/shade_editor_sheet.dart` (Update → commit + pop; Cancel → snap-back)
- `software/lamp-app-flutter/lib/features/control/presentation/widgets/base_editor_sheet.dart` (same shape)
- `software/lamp-app-flutter/lib/features/lamp_shell/presentation/lamp_shell.dart:217-329` (_SaveAction delete; AppBar actions empty)
- `software/lamp-app-flutter/lib/features/lamp_shell/presentation/setup_screen.dart` (rename + personality + home rows — mutators now call writeSettingsBlob)
- `software/lamp-app-flutter/lib/features/lamp_shell/presentation/advanced_leds_screen.dart` (Update button → writeSettingsBlob with reboot:true + verify)
- `software/lamp-app-flutter/lib/features/lamp_shell/presentation/home_mode_screen.dart` (SSID, Forget, brightness slider commits)
- `software/lamp-app-flutter/lib/features/inventory/application/inventory_notifier.dart:64-67` (drop lampSaveStatusProvider line; keep advancedSessionProvider)
- `software/lamp-app-flutter/test/features/control/application/control_notifier_test.dart` (8 rewrites)
- `software/lamp-app-flutter/test/features/lamp_shell/lamp_shell_test.dart` (3 deletes)
- `software/lamp-app-flutter/test/features/wisp/wisp_notifier_rollback_test.dart` (golden template — no change, just reference)

## Open verification items before plan write

1. Confirm `LampCrypto.encryptOp` nonce derivation (B.8). Read `software/lamp-app-flutter/lib/core/crypto/lamp_crypto.dart` or wherever encryptOp lives.
2. Confirm CHAR_COMMIT UUID. Generate fresh, add to `software/lamp-app-flutter/lib/core/ble/uuids.dart` and the firmware service-init code in sync.
3. Confirm `firmwareReceiver.isInProgress()` is callable from the loop drain context (Core 1). Should be — it's read in nearby drain blocks today.
4. Confirm whether dispositions debounce uses 5 s or different idle window; align `kCommitFlushIdleMs` with the project's convention or document why 1.5 s.

## Acceptance criteria

Phase A:
- All firmware builds (native + upesy_wroom) green
- Native tests: 272 existing + 6 new = 278 green
- Bench test #1, #2, #11, #12 pass (old-app compat, OTA interlock, cascade contamination)
- Fleet OTA from existing firmware to Phase A firmware completes cleanly on two-lamp bench

Phase B:
- `flutter analyze` clean
- Flutter tests: ~340 - deletions + new = ~342 green
- Bench tests #3-#10 pass
- Both lamps survive an hour of mixed-edit workload (rename + brightness drags + color edits + expression toggles) with no stuck state
- No `isDirty`/`_original`/`save()`/`_SaveAction`/`lampSaveStatusProvider` references remain in the lib tree (grep clean)
