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

### Internal ordering within Phase A — load-bearing

**A.1 (split user/remote mutation paths) MUST land before A.2 / A.3 in the deployment timeline.** If CHAR_COMMIT goes live in a build without the split, a mesh-relayed brightness cascade or social-greet paint can contaminate `config.*` in RAM. The first commit signal — possibly from the same user's drag on a different surface — persists the contaminated value. Bench test #12 detects this regression but not before the bad firmware has touched NVS.

Acceptable within a single PR: yes, as long as the git history orders A.1's commits before A.2's. Acceptable across PRs: A.1 must merge and the fleet must OTA-roll forward to that build before A.2's PR even ships. Treat A.1 as a hard prerequisite gate.

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

**Payload + authentication**:
- When password is unset: `WriteRouter` with `allowEmpty_=true` and `setRawAuth(isAuthed)`. App sends 1 byte (`0x01`) or 0 bytes; payload semantically ignored. The `isAuthed(connHandle)` gate is sufficient — no encrypted channel exists.
- When password is set: same `WriteRouter` slot accepts AES-GCM ciphertext framing (`0x02 + 12B nonce + ciphertext + 16B tag`). The router decrypts via `LampCrypto::decryptOp` and **verifies the GCM tag** before posting `pendingCommit = true` — inheriting the same forgery resistance as other encrypted writes. The decrypted plaintext is discarded (empty plaintext OK). Without GCM verification, an attacker who skipped the pair-bond handshake could spam commits at will.

**WriteRouter usage**: empty-payload support exists today via the `allowEmpty_` flag (`software/lamp-os/src/components/network/write_router.hpp:125-167`). Use it for CHAR_COMMIT to inherit existing crypto framing + auth integration. The earlier draft's "do not use WriteRouter" advice was based on an outdated read of the source.

```cpp
// Sketch — actual shape uses WriteRouter
auto& route = writeRouter.add(CHAR_COMMIT_UUID);
route.setAllowEmpty(true);
route.setRawAuth([](uint16_t h) { return isAuthed(h); });
route.onWrite([](const uint8_t*, size_t, uint16_t /*connHandle*/) {
  pendingCommit = true;
});
```

Post: `volatile bool pendingCommit` (single-bool naturally atomic on Xtensa; no portMUX needed). Lives file-static in `standard_lamp.cpp` next to the other `pending*` flags.

### A.3 — CHAR_COMMIT drain (loop task)

Drain in `standard_lamp.cpp`'s loop drain block. Place AFTER the per-section live-preview drains (brightness, shadeColors, baseColors, knockout, expressionOp) so the persist sees their just-mutated state. Place BEFORE the settings_blob drain so a commit and a discrete blob write in the same tick don't fight.

Debounce: file-static state in `standard_lamp.cpp` next to the other `pending*` flags. Set `commitDirty` flag + `lastCommitSignalMs` timestamp on each `pendingCommit = true` arrival. Flush only when idle ≥ `kCommitFlushIdleMs` (initial: **1500 ms** — see rationale below) OR on BLE disconnect (mirroring the existing `pendingFlushDispositionsRequested` force-flush in the BLE-disconnect handler).

This is intentionally NOT a `Config` member like `DispositionDebouncer` is. The commit flush is about *when this lamp persists*, not about Config-internal mutation state — keeping it next to the other lamp-level pending flags makes the loop-task drain readable.

On flush:
1. **Gate on `!firmwareReceiver.isInProgress()`**. A persist concurrent with OTA chunk-writes competes for the same NVS subsystem and can corrupt OTA. If OTA is in progress, defer the flush — re-check next tick.
2. Compute `currentSerialized = serializeJson(config.asJsonDocument())`.
3. Hash-dedup against `lastPersistedHash_`. If equal, skip the write — saves NVS wear on the "user dragged but ended at the same value" case.
4. `config.persistConfig(via="commit")` — see A.6 for path-tagged log.
5. On success: `config.invalidateAllSections()` + `ble_control::notifyStateChange()`, update `lastPersistedHash_ = hash(currentSerialized)`.
6. On failure: `ble_control::notifyStateChange()` (so app can detect in-RAM vs NVS skew and re-read), log loud. **Do not** update `lastPersistedHash_` — the next commit signal will retry the same payload.

**Debounce window rationale**: 1500 ms is intentionally finer-grained than the dispositions path's 5000 ms idle. Dispositions debounces against per-tick slider drags (~20 Hz BLE writes). CHAR_COMMIT receives at most one signal per natural UI fence (slider release + per-pane Update tap), so the upstream rate is already low. 1500 ms gives just enough room to coalesce "user clicks Update on shade, then base, then expressions" sequences within ~2 s into one NVS write, without making the user wait visibly for the save to land before navigating away.

### A.4 — settings_blob drain rewrite

The existing drain at `standard_lamp.cpp:1385-1490` does: parse → merge into `fullDoc` → serialize → `prefs.putString("cfg", ...)` → unconditional reboot.

New shape:

1. **factoryReset short-circuit unchanged** at the top. Add a `Serial.printf` warning if `incomingDoc.size() > 1` (factoryReset co-shipped with other keys). App-side guard already ensures this can't happen; assert is forward-defense.
2. **OTA-in-progress interlock**: if `firmwareReceiver.isInProgress()`, log loud and discard the write. App will re-issue when OTA finishes.
3. Parse → for each top-level key in incoming blob (other than `reboot` and `factoryReset`):
   - Dispatch to user-source `applyXxxLocal` helper for that section. **Skip the `expressions` key** — see A.5.
4. `config.persistConfig(via="settings_blob")`. Capture the return value as `persisted`.
5. `config.invalidateAllSections()` + `ble_control::notifyStateChange()`.
6. Reboot decision: `incomingDoc["reboot"] | true` — **default true** for backward compat. Set `lamp::fadeOutRebootRequested = true` ONLY when (a) the flag resolves true AND (b) `persisted` was true in step 4. Stale RAM is safer than rebooting into a half-applied state.

### A.5 — `applyXxxLocal` helpers (user-source variants)

New / extracted helpers:

- **`applyLampLocal(JsonObject)`** — handles `name`, `brightness`, `advancedEnabled`, `socialMode`.
  - On name change: call `NimBLEDevice::setDeviceName(newName)` so the BLE GAP device name updates without reboot. **Verify** during implementation whether `bt.tickAdvertising()` (or equivalent advertising-rebuild path) automatically picks up the new GAP name, or whether an explicit advert-payload rebuild is needed. If the latter, this helper must also trigger it. Mesh HELLO (`software/lamp-os/src/components/network/show_receiver.cpp:474`) already reads `config.lamp.name` live every 5 s, so peers see the new name on the next tick regardless of the BLE advert path.
  - On brightness: call new helper `applyBrightnessImmediate(level, isHomeMode)` that (a) updates the config field, (b) invokes `applyEffectiveBrightness()` to push the value into the strips immediately, and (c) **resets `s_userBrightnessSeeded = false`** so a subsequent live-preview slider drag re-seeds its micro-fade triple from the new persisted level instead of rubber-banding from a stale source. **Does not** seed the micro-fade triple itself — that's a slider-live-preview-only concern.
- **`applyHomeModeLocal(JsonObject)`** — handles `ssid`, `password`, `enabled`, `brightness`.
- **`applyBaseAcLocal(int)`** — tiny, for the `base.ac` field that has no live-preview char.
- **`applyBaseKnockoutLocal(JsonArray)`** — extracted from inline knockout drain logic at `standard_lamp.cpp:~1325` so settings_blob's full-section path can reuse.

**Explicitly skipped**: `applyExpressionsLocal`. The per-entry CHAR_EXPRESSION_OP path is the canonical expression mutator and already eagerly persists. A wholesale `expressions[]` overwrite from settings_blob would silently drift `expressionManager` (the live runtime) from `config.expressions` (the persisted snapshot). Document in `docs/mesh-api.md` that settings_blob ignores `expressions` and mutators must use CHAR_EXPRESSION_OP.

**Section cache**: per-section `applyXxxLocal` helpers do NOT invalidate caches inline. The single canonical `invalidateAllSections()` happens at the persist site in A.3 / A.4. Easier to reason about; matches "persist + invalidate are atomic conceptually."

### A.6 — Path-tagged `persistConfig` log

Today's log line at `software/lamp-os/src/config/config.cpp:~294`: `[nvs] persistConfig wrote N bytes`. With three callers (CHAR_COMMIT, settings_blob, CHAR_EXPRESSION_OP) all hitting this function, the log is ambiguous when fleet-debugging "did my save take?"

Change signature: `bool Config::persistConfig(const char* via)`. Log: `[nvs] persistConfig via=commit wrote 4231 bytes` / `via=settings_blob` / `via=expressionOp`. Trivial change, big debuggability win.

### A.7 — Keep CHAR_HOMEMODEFOCUS routing

The existing routing flips a flag (`s_homeModePageActive` and related) that routes CHAR_BRIGHTNESS writes to `config.homeMode.brightness` vs `config.lamp.brightness`. Implementer: grep `ble_control.cpp` for `HomeModeFocus` to find the callback (the line cite from prior drafts was stale).

With always-save, the home mode pane could write its own settings_blob with `homeMode.brightness` set, but the **live preview during a slider drag** still needs the routing — otherwise the slider in the home-mode pane would push to the wrong field. Load-bearing, keep.

**One subtle UX hazard surfaced by the protocol audit**: when the user navigates away from the home mode pane, the focus flag flips back to "lamp." If a pending CHAR_COMMIT was scheduled for the home brightness but hasn't flushed yet, the commit might persist `config.homeMode.brightness` while the user is now editing lamp brightness. This is in practice handled by the debounce-flush-on-dispose path (B.5) — when the home-mode screen disposes, the trailing commit fires before the focus flag flips. Add an explicit assertion in `applyHomeModeLocal` that the focus flag was held when the value was written, OR document this as a known race that the app-side dispose ordering must guarantee.

### A.8 — Native tests (Phase A)

All in `software/lamp-os/test/`. Native env cannot link `standard_lamp.cpp` (pulls in NimBLE, FastLED, ConfiguratorBehavior, expressionManager). Existing native tests use the **mirror-class pattern** — see `test_disposition_debounce` which redeclares `DispositionDebouncer`'s public surface in the test file to test it in isolation. Phase A native tests follow the same pattern.

- `test_commit_drain` — mirror-class redeclaration of the `commitDirty` / `lastCommitSignalMs` / `kCommitFlushIdleMs` debounce + hash-dedup logic. Verifies dirty-flag latching, hash-dedup skips identical commits, OTA interlock defers, BLE-disconnect force-flush.
- `test_settings_blob_no_reboot` — mirror-style. Verifies `{"reboot": false}` path persists but doesn't set `fadeOutRebootRequested`.
- `test_settings_blob_reboot_default` — blob without explicit `reboot` key reboots (backward compat).
- `test_settings_blob_factory_reset` — regression for the factoryReset short-circuit. Currently untested.
- `test_apply_remote_no_config_mutation` — requires extracting `applyXxxToConfig` and `applyXxxToRender` into a header-only / free-function module (a new `software/lamp-os/src/components/apply/` directory or similar) so pure native code can link. The extraction is a prerequisite of writing this test. Without it, fall back to mirror-style that asserts the invariant on a stub.
- `test_apply_brightness_immediate` — same extraction story. `applyBrightnessImmediate` must live in an extractable header so the test can call it without dragging the full lamp build in.

**Decision**: extract `applyXxxToConfig` / `applyXxxToRender` / `applyBrightnessImmediate` into `software/lamp-os/src/components/apply/` (new directory) — header-only or with a tiny implementation file. This makes Phase A's tests possible AND makes the user/remote split more visible in the codebase (it's load-bearing for cascade containment).

Acceptance for Phase A: existing 272 native tests stay green + 6 new tests, total 278 green.

## Phase B — App

Phase B is app-only — `flutter build apk --debug` + `adb install -r` for a Pixel test. No firmware changes ship with Phase B. Risk is `adb install -r`-reversible.

### Phase B preconditions

Before any Phase B code can ship to the user's Pixel:

1. **Every paired lamp the user owns must be on Phase A firmware.** Verify at connect via the existing CHAR_FW_VERSION read (`software/lamp-os/src/components/network/ble_control.cpp` exposes this). Each lamp's `controlNotifier` build path probes the firmware version; if it's pre-Phase-A, the app uses the OLD Save-pill behavior for that lamp (display the pill, route everything through `save()`). The `hasCommitChar` GATT discovery in B.3 is a redundant check — the firmware-version probe is the authoritative gate.

2. **Service discovery must expose `hasCommitChar` on the BleClient connection state.** New field. Add to `software/lamp-app-flutter/lib/core/ble/ble_client.dart` (or wherever connect/discovery lives). Populated during the post-connect service walk by checking whether CHAR_COMMIT's UUID appears in the discovered characteristic list.

3. **Mixed-fleet behavior** (some lamps Phase A, some pre-Phase A) is supported via per-lamp branching in `controlNotifier`. The `_SaveAction` pill in `lamp_shell.dart` is preserved during Phase B's transition and shown only when the watched lamp is pre-Phase-A. Deletion of the pill machinery in B.9 happens in a *follow-up* PR (call it B.9.2) after the user confirms their fleet is fully on Phase A.

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
- Define an enum in `controlNotifier`:
  ```dart
  enum CommitSection { lamp, base, shade, homeMode, baseKnockout }
  ```
- The notifier exposes `Future<void> commit(CommitSection section)`. The section parameter is local knowledge — each call site (brightness slider's `onChangeEnd`, shade editor's Update tap, etc.) knows which section it just edited.
  - `hasCommitChar == true` → write CHAR_COMMIT (single-byte sentinel / encrypted). The `section` argument is unused on the wire — the firmware persists the current RAM state regardless. The section is dead arg in this branch but kept for symmetry with the fallback.
  - `hasCommitChar == false` → fallback synthesizes a partial blob from `state.value` for the named section. Full-section payload (e.g., for `CommitSection.lamp`, ships `{"lamp": {"name": s.lamp.name, "brightness": s.lamp.brightness, "advancedEnabled": s.lamp.advancedEnabled, "socialMode": s.lamp.socialMode}}`). Writes `settingsBlob(payload, reboot: false)`. Full-section vs only-changed: full-section is cheaper to reason about — the firmware's per-section `applyXxxLocal` is idempotent on unchanged fields, so re-sending the whole section costs only the BLE write bandwidth (which is well within ATT MTU for any section the app cares about).

This keeps CHAR_COMMIT itself purely parameterless on the wire (its safety property — can't be wrong because it carries no data) while the app's fallback layer fills in the data dimension at the higher level.

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

### B.8 — Nonce-reuse — verified safe

`software/lamp-app-flutter/lib/core/crypto/lamp_crypto.dart:96` calls `_aes.encrypt(plaintext, secretKey: key)` without an explicit nonce argument. The underlying `package:cryptography` library generates a random nonce per call (AesGcm's default). Two concurrent `writeSettingsBlob` calls produce independent random nonces — no AES-GCM nonce-reuse risk.

No serialization mutex needed. Add a Flutter test asserting that two concurrent `writeSettingsBlob` calls produce distinct ciphertext bytes (regression guard for any future change to nonce derivation).

### B.9 — Deletions (split across two PRs)

Phase B's main PR introduces the new pattern but **preserves** the Save-pill machinery for pre-Phase-A lamps during the fleet transition. The deletions land in a follow-up B.9.2 PR after the user confirms their fleet is fully on Phase A firmware.

**B.9.1 — Stays in main Phase B PR (gated on per-lamp `hasCommitChar` flag):**
- `_SaveAction` widget rendering — show only when `hasCommitChar == false` for the active lamp
- `controlNotifier.save()` — invoked only when `hasCommitChar == false`
- `isDirty` getter — still consumed by the pill on pre-Phase-A lamps
- `_isLampDirty`, etc. — still needed by isDirty's implementation
- `_original` snapshot — still needed by isDirty's diff
- `_awaitReconnectAndReload` — still needed by save() path
- `lampSaveStatusProvider` — still drives the connecting-view "Saving changes…" branch on pre-Phase-A lamps
- "Saving changes…" branch in `connecting_view.dart`

**B.9.2 — Follow-up PR (after fleet fully on Phase A):**
- All of the above, plus:
- `lamp_save_status.dart` + `.g.dart` (full delete)
- `inventory_notifier.dart`'s `_invalidatePerLampState` line that touches `lampSaveStatusProvider(id)` (just drop that line, keep `advancedSessionProvider(id)`)
- `connecting_view.dart` reverts to plain Connecting state

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
4. **Phase B: brightness commit-during-debounce loss window**: drag, release, power-cycle within 500 ms. Document the loss. **Accepted limitation** — the alternative (no debounce, every onChangeEnd commits immediately) would trigger an NVS write on every tap-to-position, and the debounce already commits on BLE disconnect (B.5). A USB-unplug-within-500-ms-of-release window is intentionally outside the persist guarantee. Listed in "Deferred / documented" with rationale.
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
- **500 ms brightness loss window**: if the user yanks lamp power within ~500 ms of releasing the slider, the debounce hasn't fired and the change is lost. Documented in bench test #4. Mitigated by BLE-disconnect force-flush (B.5) — a graceful disconnect (app backgrounded, BT off) flushes the pending commit before disconnect; only a hard power-cut hits this window.
- **Coalesced live-preview write failures bubble silently**: brightness/colors/knockout live-preview writes via `WriteCoalescer` schedule async; if the deferred write fails, the existing `safeWrite` swallows non-disconnect errors and kicks the reconnect ladder on disconnect. The `_mutate` wrap covers the explicit commit call but NOT the live-preview drag-tick failures. Acceptable per current contract (matches today's wisp `setOffColor` behavior). Document in `controlNotifier`'s class doc.

## File map (critical files)

**Firmware (Phase A):**
- `software/lamp-os/src/components/apply/` (**new directory** — extracted `applyXxxToConfig`, `applyXxxToRender`, `applyBrightnessImmediate` helpers, header-only or with a small `.cpp` to keep them native-testable)
- `software/lamp-os/src/lamps/standard_lamp.cpp:407-472` (applyExpressionOpLocal — splits into ToConfig/ToRender via the new apply/ module)
- `software/lamp-os/src/lamps/standard_lamp.cpp:487-545` (applyRemoteOpLocal — calls only ToRender variants after the split)
- `software/lamp-os/src/lamps/standard_lamp.cpp:1219-1490` (full drain block; CHAR_COMMIT drain inserted after live-preview drains and before settings_blob; settings_blob rewritten)
- `software/lamp-os/src/components/network/ble_control.cpp` (register CHAR_COMMIT in the service-init code; reuse WriteRouter with `allowEmpty=true` + `setRawAuth(isAuthed)`. SettingsBlobCallback receives `reboot` flag.)
- `software/lamp-os/src/components/network/write_router.hpp:125-167` (existing `allowEmpty_` support — reused, not modified)
- `software/lamp-os/src/config/config.hpp:84-105` (Config::persistConfig signature: `bool persistConfig(const char* via)`)
- `software/lamp-os/src/config/config.cpp:272-298` (persistConfig path-tagged log; update call sites in the same commit as the signature change — see verification item #6)
- `software/lamp-os/src/components/network/show_receiver.cpp:474` (mesh HELLO reads name live — confirm)
- `software/lamp-os/test/test_commit_drain/`, `test_settings_blob_*/`, `test_apply_*/` (new test directories per A.8)

**App (Phase B):**
- `software/lamp-app-flutter/lib/core/ble/ble_client.dart` (add `hasCommitChar` to connection state; populate during service discovery — Phase B's FIRST task)
- `software/lamp-app-flutter/lib/core/ble/uuids.dart` (add `commit` UUID — same constant the firmware registers)
- `software/lamp-app-flutter/lib/features/control/application/control_notifier.dart` (everything — new helpers, mutator rewrites, deletions, `CommitSection` enum)
- `software/lamp-app-flutter/lib/features/control/application/lamp_save_status.dart` (delete *in B.9.2 follow-up*, not in main Phase B — still needed for pre-Phase-A-firmware lamps during transition)
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

1. ~~Confirm `LampCrypto.encryptOp` nonce derivation.~~ **Resolved**: package:cryptography's AesGcm uses random nonce per call. See B.8.
2. Generate CHAR_COMMIT UUID. Add to `software/lamp-app-flutter/lib/core/ble/uuids.dart` and the firmware service-init code in sync.
3. Verify `firmwareReceiver.isInProgress()` is callable from the loop drain context (Core 1). Should be — it's read in nearby drain blocks today.
4. ~~Confirm debounce window.~~ **Resolved**: 1500 ms documented with rationale (A.3).
5. Verify `NimBLEDevice::setDeviceName` change is picked up by the existing advertisement-rebuild path, or whether `applyLampLocal` must explicitly trigger an advert refresh. Read `bt.tickAdvertising()` and adjacent advertisement code.
6. Inventory the call sites of today's `Config::persistConfig()` (currently 2 — one in expressionOp drain from earlier today, the dispositions path doesn't use it). All must update to pass the new `via` parameter in the same commit as the signature change.
7. Verify `_isExpressionsDirty` deletion doesn't break expression editor's dirty-pill (the editor screen has its own draft-vs-saved tracking separate from `controlNotifier.isDirty`; that draft state is independent of `_isExpressionsDirty`, but worth grepping `_isExpressionsDirty` callers before deletion).
8. Confirm extraction location for `applyXxxToConfig` / `applyBrightnessImmediate` — proposed `software/lamp-os/src/components/apply/` — fits the existing component layout. If a more idiomatic location exists, use that.

## Acceptance criteria

Phase A:
- All firmware builds (native + upesy_wroom) green
- Native tests: 272 existing + 6 new = 278 green
- Bench test #1, #2, #11, #12 pass (old-app compat, OTA interlock, cascade contamination)
- Fleet OTA from existing firmware to Phase A firmware completes cleanly on two-lamp bench

Phase B (B.9.1 main PR):
- `flutter analyze` clean
- Flutter tests: ~340 - rewrites + new = ~342 green
- Bench tests #3-#10 pass on lamps running Phase A firmware
- Bench test #2 still passes for any pre-Phase-A lamp on the bench (old Save-pill path works)
- Both lamps survive an hour of mixed-edit workload (rename + brightness drags + color edits + expression toggles) with no stuck state
- `controlNotifier.commit()` and `writeSettingsBlob` exist with full per-pane wiring
- `_SaveAction` pill renders ONLY for lamps where `hasCommitChar == false`

Phase B.9.2 follow-up (deletion PR, after fleet fully on Phase A):
- No `isDirty`/`_original`/`save()`/`_SaveAction`/`lampSaveStatusProvider` references remain in the lib tree (grep clean)
- `flutter analyze` clean
- Flutter tests: full ~342 green (the pre-Phase-A path tests are deleted alongside the code they exercise)
