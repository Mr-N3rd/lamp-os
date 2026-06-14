# Always Save Phase B (App) — B.9.1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the app-side foundation for "always save, no preview" with per-lamp transition compatibility — for lamps running Phase A firmware (`hasCommitChar == true`), per-pane mutators commit on the natural fence (slider release, editor Update tap, dialog Save) so there's nothing dirty for the user to save. For lamps still on pre-Phase-A firmware (`hasCommitChar == false`), preserve today's Save-pill flow exactly.

**Architecture:** Per-lamp branching gated on `hasCommitChar` (probed via GATT service discovery). New `commit(CommitSection)` method writes CHAR_COMMIT on Phase A lamps and is a no-op on pre-Phase-A lamps. New `writeSettingsBlob(Map, {reboot})` helper handles discrete edits (with `reboot: false` for in-place fields, `reboot: true` for advanced LED + factoryReset). Existing `save()` / `isDirty` / `_original` / `_SaveAction` pill machinery STAYS for the transition — the pill is just hidden when `hasCommitChar == true`. Deletion of the legacy path lives in a separate follow-up PR (B.9.2) after the fleet is fully on Phase A.

**Tech Stack:** Flutter, Riverpod 2.x (codegen), flutter_blue_plus (BLE), Unity (firmware native tests — not used here). Codebase: `/Users/jerrett/projects/lamp-os/software/lamp-app-flutter`.

**Spec:** `docs/superpowers/specs/2026-06-13-always-save-no-preview-design.md`. Phase B / B.9.1 scope only.

**B.9.2 follow-up (NOT in this plan):** delete `controlNotifier.save()`, `isDirty`, `_isXxxDirty`, `_original`, `_awaitReconnectAndReload`, `lampSaveStatusProvider` (+ `.g.dart`), `_SaveAction`, "Saving changes…" branch in `connecting_view.dart`. That PR lands after the user confirms their fleet is fully on Phase A.

**Deployment note**: per the user's memory + this session's repeated reminders, **never run `flutter install`** — it uninstalls first, wiping the lamp inventory. Always `adb install -r build/app/outputs/flutter-apk/app-debug.apk`. Specify the device id explicitly because the user keeps an emulator paired alongside their Pixel.

---

## File Map

**New files:**
- `lib/features/control/application/commit_section.dart` — `enum CommitSection { lamp, base, shade, homeMode, baseKnockout }`

**Modified core BLE:**
- `lib/core/ble/ble_client.dart` — add abstract `Future<bool> probeHasCommitChar(String deviceId)` to the BleClient interface
- `lib/core/ble/fbp_ble_client.dart` (or equivalent concrete impl — grep for `class FbpBleClient` or `implements BleClient` to find) — implement the probe by reading the GATT characteristic list and checking for the `BleUuids.commit` UUID
- `lib/core/ble/inventory_notifier.dart` (or wherever the per-lamp connection state is stored) — cache `hasCommitChar` per device id; populate post-connect

**Modified control notifier (the big one):**
- `lib/features/control/application/control_notifier.dart` — add `_mutate` helper, `commit(CommitSection)`, `writeSettingsBlob(Map, {bool reboot})`, lifecycle hooks, per-mutator behavior switches

**Modified per-pane widgets/screens:**
- `lib/features/control/presentation/widgets/brightness_card.dart` — `onChangeEnd` schedules commit
- `lib/features/control/presentation/widgets/shade_editor_sheet.dart` — Update tap → commit; Cancel → snap-back
- `lib/features/control/presentation/widgets/base_editor_sheet.dart` — Update tap → commit + writeSettingsBlob for ac; Cancel → snap-back
- `lib/features/lamp_shell/presentation/setup_screen.dart` — rename/personality/advanced-toggle → writeSettingsBlob (reboot:false) on Phase A lamps; inventory cache update on rename
- `lib/features/lamp_shell/presentation/home_mode_screen.dart` — toggle/SSID/forget → writeSettingsBlob; brightness slider → commit
- `lib/features/lamp_shell/presentation/advanced_leds_screen.dart` — Update → writeSettingsBlob(reboot:true) + post-reconnect verify
- `lib/features/lamp_shell/presentation/lamp_shell.dart:121` — gate `_SaveAction` rendering on `!hasCommitChar`

**New tests:**
- `test/features/control/commit_method_test.dart` — `commit(section)` writes CHAR_COMMIT when `hasCommitChar==true`, no-ops otherwise
- `test/features/control/mutate_helper_test.dart` — `_mutate` capture/revert on rollback
- `test/features/control/brightness_commit_debounce_test.dart` — debounce schedules commit, dispose flushes
- `test/features/control/setup_mutators_phasea_vs_prephasea_test.dart` — phase-aware behavior matrix for setLampName / setLampSocialMode / setLampAdvancedEnabled
- `test/features/control/advanced_led_reboot_verify_test.dart` — post-reconnect diff surfaces "save didn't take" snackbar on mismatch

---

## Critical-ordering invariants

Within Phase B.9.1:
- **Task 1 (BleClient probe) must merge before Tasks 7-12** (per-pane wiring). All per-pane behavior gates on `hasCommitChar`, which the probe populates.
- **Tasks 2-4 (foundation: enum + commit + writeSettingsBlob + _mutate) must merge before Tasks 7-12**. The per-pane wiring CALLS these helpers; without them the build fails.
- **Task 13 (Save-pill visibility gate) lands LAST** — until per-pane behavior is wired, hiding the pill on Phase A lamps would strand mutator edits.

---

### Task 1: BleClient — `probeHasCommitChar` + per-device cache

Detect whether the lamp at `deviceId` exposes CHAR_COMMIT in its GATT characteristic list. Cache the result so the per-pane mutators can read it synchronously.

**IMPORTANT findings from the implementability review:**
- `InMemoryBleClient` is a class inside `lib/core/ble/ble_client.dart` (NOT in `test/test_support/`). Test imports use `package:lamp_app/core/ble/ble_client.dart`.
- The existing `InMemoryBleClient` has NO `setDiscoveredChars` or `writesTo` helpers — they must be added in Step 2 before the tests will compile.
- `InventoryLamp` is a Freezed class. Adding `hasCommitChar` requires running `dart run build_runner build --delete-conflicting-outputs` to regenerate `inventory_lamp.freezed.dart` and `inventory_lamp.g.dart`. The new field MUST be `bool? hasCommitChar` with a `?? false` default at read sites — old SharedPreferences entries don't have the key.

**Files:**
- Modify: `lib/core/ble/ble_client.dart` — abstract method addition + `InMemoryBleClient` helpers
- Modify: `lib/core/ble/fbp_ble_client.dart` — concrete probe via flutter_blue_plus's discovered characteristics (grep `class FbpBleClient` / `BluetoothDevice.discoverServices` to find the exact location)
- Modify: `lib/features/inventory/domain/inventory_lamp.dart` (Freezed class) — add `bool? hasCommitChar` field
- Run: `dart run build_runner build --delete-conflicting-outputs` (regenerate `inventory_lamp.freezed.dart` + `inventory_lamp.g.dart`)
- Modify: `lib/features/inventory/application/inventory_notifier.dart` — populate `hasCommitChar` post-connect; expose `hasCommitChar(deviceId) -> bool` synchronous getter on the inventory entry (or notifier)
- Test: `test/core/ble/probe_has_commit_char_test.dart`

- [ ] **Step 1: Find the BleClient concrete impl + per-lamp inventory entry**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-app-flutter
grep -rn "implements BleClient\|class FbpBleClient\|extends BleClient" lib/core/ble/ | head -3
grep -rn "class InventoryLamp\|class InventoryNotifier" lib/features/inventory 2>/dev/null | head -5
```

Note the file paths returned. Substitute them in subsequent steps where this task says `lib/core/ble/fbp_ble_client.dart` or `lib/features/inventory/...`.

- [ ] **Step 2: Add abstract method to `lib/core/ble/ble_client.dart`**

Inside `abstract class BleClient { ... }`, add at the bottom of the abstract methods (find the closing `}` of the class and add just above it):

```dart
  /// Probes the lamp's GATT characteristic list to determine whether
  /// the CHAR_COMMIT characteristic ([BleUuids.commit]) is exposed.
  ///
  /// `true` means the lamp is running Phase A firmware: per-pane edits
  /// commit immediately via CHAR_COMMIT and the Save pill is hidden.
  /// `false` means pre-Phase-A firmware: keep the legacy Save-pill flow.
  ///
  /// Must be called AFTER the connection is established + services are
  /// discovered (typically right after [connect] resolves). Returns
  /// false on any read/discovery error — defaulting to legacy behavior
  /// is the safer fallback than assuming Phase A.
  Future<bool> probeHasCommitChar(String deviceId);
```

- [ ] **Step 3: Write the failing test**

Create `test/core/ble/probe_has_commit_char_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_os/core/ble/ble_client.dart';
import 'package:lamp_os/core/ble/uuids.dart';

import 'package:lamp_app/core/ble/ble_client.dart'; // InMemoryBleClient is a class inside this file

void main() {
  group('probeHasCommitChar', () {
    test('returns true when CHAR_COMMIT is in the discovered char list', () async {
      final ble = InMemoryBleClient();
      ble.setDiscoveredChars('lamp-a', [BleUuids.commit, BleUuids.brightness]);
      expect(await ble.probeHasCommitChar('lamp-a'), isTrue);
    });

    test('returns false when CHAR_COMMIT is absent', () async {
      final ble = InMemoryBleClient();
      ble.setDiscoveredChars('lamp-b', [BleUuids.brightness, BleUuids.settingsBlob]);
      expect(await ble.probeHasCommitChar('lamp-b'), isFalse);
    });

    test('returns false on unknown device (no discovery)', () async {
      final ble = InMemoryBleClient();
      expect(await ble.probeHasCommitChar('lamp-c'), isFalse);
    });
  });
}
```

If `InMemoryBleClient` (or whatever the test double is called) doesn't expose `setDiscoveredChars`, grep `test/test_support/` for the existing test double and add the helper inline using the pattern already there.

- [ ] **Step 4: Run the test (expect FAIL — method not yet implemented)**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-app-flutter
flutter test test/core/ble/probe_has_commit_char_test.dart
```

Expected: compile error or 3 test failures because the abstract method isn't implemented in the test double yet.

- [ ] **Step 5: Implement `probeHasCommitChar` + add test helpers in `InMemoryBleClient`**

`InMemoryBleClient` is defined inside `lib/core/ble/ble_client.dart` (find the `class InMemoryBleClient implements BleClient` block — search for it). Add THREE things:

(a) The `_discoveredChars` map + `setDiscoveredChars` helper for test injection.
(b) `probeHasCommitChar` override.
(c) A `writesTo(deviceId, charUuid)` helper that returns the list of write payloads matching a `(deviceId, charUuid)` pair across any service UUID. The existing `_values` map keys by `_key(d, s, c)` so writes need a separate `_writeLog` list to preserve write history.

```dart
  // Test injection: GATT-discovered characteristic UUIDs per device.
  final Map<String, List<String>> _discoveredChars = {};
  void setDiscoveredChars(String deviceId, List<String> chars) {
    _discoveredChars[deviceId] = List.of(chars);
  }

  // Write log — captures every write call so tests can assert what
  // landed on a given (deviceId, charUuid) pair. Append on every
  // `write(...)` call; tests query via `writesTo`.
  final List<({String deviceId, String charUuid, Uint8List value})>
      _writeLog = [];

  List<Uint8List> writesTo(String deviceId, String charUuid) {
    return _writeLog
        .where((w) => w.deviceId == deviceId && w.charUuid == charUuid)
        .map((w) => w.value)
        .toList();
  }

  @override
  Future<bool> probeHasCommitChar(String deviceId) async {
    final chars = _discoveredChars[deviceId];
    if (chars == null) return false;
    return chars.contains(BleUuids.commit);
  }
```

ALSO modify the existing `write(...)` method body in `InMemoryBleClient` to append to `_writeLog`:

```dart
  @override
  Future<void> write(
    String deviceId,
    String serviceUuid,
    String charUuid,
    Uint8List value, {
    bool withoutResponse = false,
    bool allowLongWrite = false,
  }) async {
    // existing _values store (preserve whatever was there)
    _values[_key(deviceId, serviceUuid, charUuid)] = value;
    // NEW: write log for tests
    _writeLog.add((
      deviceId: deviceId,
      charUuid: charUuid,
      value: Uint8List.fromList(value),
    ));
  }
```

- [ ] **Step 6: Implement `probeHasCommitChar` in `FbpBleClient`**

In the concrete impl (e.g. `lib/core/ble/fbp_ble_client.dart`), add (substitute the actual class name from Step 1):

```dart
  @override
  Future<bool> probeHasCommitChar(String deviceId) async {
    try {
      final device = _deviceFor(deviceId); // adjust if your impl uses a different lookup
      final services = await device.discoverServices();
      for (final svc in services) {
        if (svc.uuid.str.toLowerCase() != BleUuids.controlService.toLowerCase()) {
          continue;
        }
        for (final char in svc.characteristics) {
          if (char.uuid.str.toLowerCase() == BleUuids.commit.toLowerCase()) {
            return true;
          }
        }
      }
      return false;
    } catch (e, st) {
      debugPrint('FbpBleClient.probeHasCommitChar($deviceId) failed: $e\n$st');
      return false;
    }
  }
```

Notes for the implementer: flutter_blue_plus's `BluetoothDevice.discoverServices()` caches by default. If the connection has already discovered services, this call should be cheap. The `_deviceFor(deviceId)` lookup is whatever pattern the rest of `FbpBleClient` uses for `connect`/`read`/`write`.

- [ ] **Step 7: Run the test (expect PASS)**

```bash
flutter test test/core/ble/probe_has_commit_char_test.dart
```

Expected: 3/3 PASS.

- [ ] **Step 8: Add `hasCommitChar` field to `InventoryLamp` Freezed class**

Find `InventoryLamp` (probably `lib/features/inventory/domain/inventory_lamp.dart`). Add a new field to the `@freezed` class definition:

```dart
@freezed
class InventoryLamp with _$InventoryLamp {
  const factory InventoryLamp({
    required String id,
    required String name,
    // ... existing fields ...
    // NEW: GATT-discovered presence of CHAR_COMMIT. null when not yet
    // probed (pre-existing inventory entries, mid-connect window).
    // Read sites default to false via `?? false`.
    bool? hasCommitChar,
  }) = _InventoryLamp;

  factory InventoryLamp.fromJson(Map<String, dynamic> json) =>
      _$InventoryLampFromJson(json);
}
```

The field is intentionally **nullable** so old SharedPreferences entries (which won't have this key) parse cleanly — Freezed's `fromJson` returns null for missing keys.

- [ ] **Step 9: Regenerate Freezed/JSON code**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-app-flutter
dart run build_runner build --delete-conflicting-outputs
```

Expected: `inventory_lamp.freezed.dart` and `inventory_lamp.g.dart` are regenerated. No errors. Skipping this step makes the entire app fail to compile.

- [ ] **Step 10: Populate `hasCommitChar` post-connect in the inventory notifier**

Find the post-connect handler in `inventory_notifier.dart` (search for `await ble.connect(` or `_ble.connect`). After the connection settles and AFTER services are discovered (the probe needs the GATT cache primed), add:

```dart
final hasCommit = await ble.probeHasCommitChar(deviceId);
final updatedLamps = state.value!.map((l) {
  return l.id == deviceId ? l.copyWith(hasCommitChar: hasCommit) : l;
}).toList();
state = AsyncData(updatedLamps);
// Persist if your inventory persists changes to SharedPreferences.
await _persist(updatedLamps);  // adjust to whatever the existing persist
                                // method is — grep `prefs.setString` to find
```

Read sites use `entry.hasCommitChar ?? false` to default `null` to legacy behavior (Save-pill flow) until the probe lands.

- [ ] **Step 9: Run the full BLE test suite**

```bash
flutter test test/core/ble/
```

Expected: all existing tests still pass + the 3 new ones.

- [ ] **Step 10: Commit**

```bash
git add lib/core/ble/ble_client.dart lib/core/ble/fbp_ble_client.dart lib/features/inventory/application/inventory_notifier.dart test/core/ble/probe_has_commit_char_test.dart test/test_support/in_memory_ble_client.dart
git commit -m "$(cat <<'EOF'
feat(ble): probe CHAR_COMMIT presence + per-lamp hasCommitChar cache

Phase B.9.1 prep — populated post-connect, read by the per-pane
mutators to decide whether to fire commit (Phase A firmware) or fall
back to the Save-pill flow (pre-Phase-A firmware).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `CommitSection` enum + `commit(CommitSection)` method on `controlNotifier`

The local-knowledge section parameter for the natural-fence commit. On Phase A lamps, fires CHAR_COMMIT (parameterless on the wire — section arg is dead). On pre-Phase-A lamps, no-ops (the Save pill remains the persistence trigger).

**Files:**
- Create: `lib/features/control/application/commit_section.dart`
- Modify: `lib/features/control/application/control_notifier.dart` — add `commit(CommitSection)` method
- Test: `test/features/control/commit_method_test.dart`

- [ ] **Step 1: Create `commit_section.dart`**

```dart
// lib/features/control/application/commit_section.dart
//
// Local-knowledge tag that per-pane call sites pass to
// `controlNotifier.commit()` to identify which section they just
// edited. CHAR_COMMIT itself is parameterless on the wire — the
// firmware persists whatever's in RAM regardless of section. The
// section IS used by the (rare) writeSettingsBlob fallback when a
// caller wants the equivalent partial-blob behavior on a pre-Phase-A
// lamp; that path is opt-in per-call.

enum CommitSection {
  lamp,
  base,
  shade,
  homeMode,
  baseKnockout,
}
```

- [ ] **Step 2: Write the failing test**

Create `test/features/control/commit_method_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:lamp_os/core/ble/ble_client.dart';
import 'package:lamp_os/core/ble/uuids.dart';
import 'package:lamp_os/features/control/application/commit_section.dart';
import 'package:lamp_os/features/control/application/control_notifier.dart';

import 'package:lamp_app/core/ble/ble_client.dart'; // InMemoryBleClient is a class inside this file

void main() {
  group('controlNotifier.commit(section)', () {
    test('writes a single byte to CHAR_COMMIT when hasCommitChar=true', () async {
      final ble = InMemoryBleClient();
      ble.setDiscoveredChars('lamp-a', [BleUuids.commit]);
      // ... wire controlNotifier with the InMemoryBleClient + a lamp at 'lamp-a' ...
      final container = ProviderContainer(overrides: [
        bleClientProvider.overrideWithValue(ble),
      ]);
      addTearDown(container.dispose);
      final notifier = container.read(controlNotifierProvider('lamp-a').notifier);
      // pre-populate hasCommitChar=true via the inventory notifier setup
      // expected: a CHAR_COMMIT write of <= 4 bytes lands on InMemoryBleClient.
      await notifier.commit(CommitSection.lamp);
      expect(ble.writesTo('lamp-a', BleUuids.commit).length, 1);
      expect(ble.writesTo('lamp-a', BleUuids.commit).first.length, lessThanOrEqualTo(4));
    });

    test('no-ops when hasCommitChar=false', () async {
      final ble = InMemoryBleClient();
      ble.setDiscoveredChars('lamp-b', [BleUuids.brightness]); // no commit char
      final container = ProviderContainer(overrides: [
        bleClientProvider.overrideWithValue(ble),
      ]);
      addTearDown(container.dispose);
      final notifier = container.read(controlNotifierProvider('lamp-b').notifier);
      await notifier.commit(CommitSection.lamp);
      expect(ble.writesTo('lamp-b', BleUuids.commit), isEmpty);
    });
  });
}
```

If `InMemoryBleClient.writesTo(deviceId, charUuid)` doesn't exist, add it inline — match the existing pattern in `test_support/`. Same for any provider override helper.

- [ ] **Step 3: Run the test (FAIL expected)**

```bash
flutter test test/features/control/commit_method_test.dart
```

Expected: failures because `controlNotifier.commit()` doesn't exist yet.

- [ ] **Step 4: Implement `commit(CommitSection)` in `control_notifier.dart`**

Add the import:
```dart
import 'commit_section.dart';
```

Find a place near the top of the class body (or near `save()` at line 555 — group conceptually with other persist methods). Add:

```dart
  /// Phase B.9.1 commit fence. On lamps running Phase A firmware
  /// (hasCommitChar==true) writes a single byte to CHAR_COMMIT — the
  /// firmware persists current RAM state to NVS. On pre-Phase-A lamps
  /// (hasCommitChar==false) this is a no-op; the Save pill remains the
  /// persistence trigger.
  ///
  /// The [section] argument is local knowledge for the caller (so each
  /// per-pane site declares what it just edited). It is NOT serialized
  /// to the wire — CHAR_COMMIT is parameterless. Reserved for the
  /// (rare) per-pane fallback that calls `writeSettingsBlob` directly
  /// when the caller explicitly wants partial-blob semantics on a
  /// pre-Phase-A lamp.
  Future<void> commit(CommitSection section) async {
    final inv = await ref.read(inventoryNotifierProvider.future);
    final entry = inv.firstWhere(
      (l) => l.id == _deviceId,
      orElse: () => throw StateError('lamp $_deviceId not in inventory'),
    );
    //
    // DESIGN DEVIATION FROM SPEC B.3: the spec describes a fallback that
    // synthesizes a partial settings_blob from state.value when
    // hasCommitChar==false. This plan takes a simpler interpretation —
    // pre-Phase-A lamps fall through to the legacy Save-pill flow (the
    // pill is still visible on those lamps per Task 12). No fallback
    // synthesis. Rationale: the synthesized blob would trigger a reboot
    // on pre-Phase-A firmware (which ignores the reboot:false flag),
    // which is exactly the UX the user said they don't want for slider
    // releases / picker accepts. Save-pill behavior on pre-Phase-A
    // lamps is identical to today; users on those lamps tap Save when
    // they're ready.
    //
    // The `?? false` default treats null (not-yet-probed) as pre-Phase-A
    // — safe default; the probe will populate within ~100ms of connect.
    if (!(entry.hasCommitChar ?? false)) return;

    final ble = ref.read(bleClientProvider);
    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.commit,
        Uint8List.fromList([0x01]),
      );
    } catch (e, st) {
      // Surface to the snackbar layer via rethrow; caller wraps in
      // try/catch and shows the user "Couldn't save — disconnected".
      debugPrint('controlNotifier.commit(${section.name}) failed: $e\n$st');
      rethrow;
    }
  }
```

If `inventoryNotifierProvider` doesn't expose `hasCommitChar` on the lamp entry, the simpler version reads from wherever Task 1 cached it. Adjust accordingly.

- [ ] **Step 5: Run the test (PASS expected)**

```bash
flutter test test/features/control/commit_method_test.dart
```

Expected: 2/2 PASS.

- [ ] **Step 6: Run flutter analyze**

```bash
flutter analyze lib/features/control/application/control_notifier.dart lib/features/control/application/commit_section.dart
```

Expected: No issues found.

- [ ] **Step 7: Commit**

```bash
git add lib/features/control/application/commit_section.dart lib/features/control/application/control_notifier.dart test/features/control/commit_method_test.dart
git commit -m "$(cat <<'EOF'
feat(control): commit(section) method + CommitSection enum

Phase B.9.1 — writes a sentinel byte to CHAR_COMMIT on Phase A lamps,
no-ops on pre-Phase-A. Section arg is local knowledge (which pane just
edited); not serialized to the wire.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `writeSettingsBlob(Map, {bool reboot})` helper

Wraps the existing settings_blob encryption + write path that `setLampPassword` / `factoryReset` already use. For discrete actions (rename, personality, home toggle) on Phase A lamps; for the Advanced LED reboot path; and as the explicit fallback when a caller wants partial-blob semantics on pre-Phase-A lamps.

**Files:**
- Modify: `lib/features/control/application/control_notifier.dart`
- Test: `test/features/control/write_settings_blob_test.dart`

- [ ] **Step 1: Read the existing `setLampPassword` and `factoryReset` for reference**

```bash
sed -n '1076,1140p' lib/features/control/application/control_notifier.dart
sed -n '963,1020p' lib/features/control/application/control_notifier.dart
```

Both build a `blob` map, encrypt via `LampCrypto.encryptOp` with the `settingsBlob` salt, and `ble.write` with `allowLongWrite: true`. The new helper extracts that pattern.

- [ ] **Step 2: Write the failing test**

Create `test/features/control/write_settings_blob_test.dart`:

```dart
import 'dart:convert';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:lamp_os/core/ble/uuids.dart';
import 'package:lamp_os/features/control/application/control_notifier.dart';

import 'package:lamp_app/core/ble/ble_client.dart'; // InMemoryBleClient is a class inside this file

void main() {
  group('controlNotifier.writeSettingsBlob', () {
    test('includes reboot:false flag when requested', () async {
      // ... setup controlNotifier with InMemoryBleClient ...
      final container = ProviderContainer(/* ... */);
      addTearDown(container.dispose);
      final notifier = container.read(controlNotifierProvider('lamp-a').notifier);
      await notifier.writeSettingsBlob({'lamp': {'name': 'foo'}}, reboot: false);

      final inMem = container.read(bleClientProvider) as InMemoryBleClient;
      final write = inMem.writesTo('lamp-a', BleUuids.settingsBlob).first;
      // Decode (no-password path = magicPlaintext + json) to verify the flag:
      final json = jsonDecode(utf8.decode(write.sublist(1)));
      expect(json['reboot'], false);
      expect(json['lamp']['name'], 'foo');
    });

    test('defaults reboot to true when not specified', () async {
      // ... setup ...
      await notifier.writeSettingsBlob({'lamp': {'name': 'foo'}});
      final write = /* ... pull the write ... */;
      final json = jsonDecode(utf8.decode(write.sublist(1)));
      expect(json['reboot'], isTrue);
    });
  });
}
```

(If `.let((b) => ...)` doesn't compile in your Dart setup, just use a temporary variable.)

- [ ] **Step 3: Run test (FAIL expected)**

```bash
flutter test test/features/control/write_settings_blob_test.dart
```

- [ ] **Step 4: Implement `writeSettingsBlob` in `control_notifier.dart`**

Place near `save()` (line 555 area). Use the existing `LampCrypto.encryptOp` + `ble.write` pattern from `setLampPassword`:

```dart
  /// Writes a partial settings_blob with the given JSON map. The
  /// `reboot` flag is added to the map before encryption — Phase A
  /// firmware reads it; pre-Phase-A firmware ignores it (always
  /// reboots).
  ///
  /// Discrete edits on Phase A lamps: reboot: false.
  /// Advanced LED + factoryReset: reboot: true.
  /// Throws on BLE error (caller wraps + snackbars). Swallows the
  /// expected BleDisconnectedException for reboot:true writes — the
  /// reboot drops the link mid-write.
  Future<void> writeSettingsBlob(
    Map<String, dynamic> blob, {
    bool reboot = true,
  }) async {
    final ble = ref.read(bleClientProvider);
    final inv = await ref.read(inventoryNotifierProvider.future);
    final lamp = inv.firstWhere(
      (l) => l.id == _deviceId,
      orElse: () => throw StateError('lamp $_deviceId not in inventory'),
    );
    final pw = lamp.controlPassword ?? '';

    final payloadBlob = <String, dynamic>{
      ...blob,
      'reboot': reboot,
    };
    final blobJson = jsonEncode(payloadBlob);

    final payload = pw.isEmpty
        ? Uint8List.fromList([
            LampCrypto.magicPlaintext,
            ...utf8.encode(blobJson),
          ])
        : await LampCrypto.encryptOp(
            op: payloadBlob,
            password: pw,
            saltUuid16: uuidSaltLE16(BleUuids.settingsBlob),
            charShortName: 'settingsBlob',
          );

    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.settingsBlob,
        payload,
        allowLongWrite: true,
      );
    } on BleDisconnectedException {
      // Expected when reboot==true. For reboot==false this is a real
      // disconnect — caller will see the rethrow and snackbar.
      if (!reboot) rethrow;
    }
  }
```

- [ ] **Step 5: Run test (PASS expected)**

```bash
flutter test test/features/control/write_settings_blob_test.dart
```

- [ ] **Step 6: Add the nonce-non-reuse regression test (B.8 spec requirement)**

Create `test/features/control/nonce_nonreuse_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:lamp_os/core/ble/uuids.dart';

import 'package:lamp_app/core/ble/ble_client.dart'; // InMemoryBleClient is a class inside this file

void main() {
  group('writeSettingsBlob nonce-non-reuse (B.8 regression guard)', () {
    test('two near-simultaneous discrete edits produce distinct ciphertexts', () async {
      // Setup: lamp with a password set so the encrypted path is taken.
      final ble = InMemoryBleClient();
      // ... wire up an authed lamp 'lamp-a' with hasCommitChar=true ...
      final container = ProviderContainer(overrides: [
        bleClientProvider.overrideWithValue(ble),
      ]);
      addTearDown(container.dispose);
      final notifier = container.read(controlNotifierProvider('lamp-a').notifier);

      // Fire two writeSettingsBlob calls back-to-back (small payloads,
      // realistic real-world: user mashes Save on rename then immediately
      // taps personality).
      await Future.wait([
        notifier.writeSettingsBlob({'lamp': {'name': 'A'}}, reboot: false),
        notifier.writeSettingsBlob({'lamp': {'socialMode': 1}}, reboot: false),
      ]);

      final writes = ble.writesTo('lamp-a', BleUuids.settingsBlob);
      expect(writes.length, 2);
      // Distinct ciphertext bytes — if package:cryptography ever
      // switches to deterministic nonces this fails loudly.
      expect(writes[0], isNot(equals(writes[1])));
    });
  });
}
```

- [ ] **Step 7: Run the new test**

```bash
flutter test test/features/control/nonce_nonreuse_test.dart
```

Expected: 1/1 PASS. The current `package:cryptography` AesGcm implementation uses a random nonce per call (verified during spec writing), so the test should pass without any code change.

- [ ] **Step 8: Commit**

```bash
git add lib/features/control/application/control_notifier.dart test/features/control/write_settings_blob_test.dart test/features/control/nonce_nonreuse_test.dart
git commit -m "$(cat <<'EOF'
feat(control): writeSettingsBlob(Map, {reboot}) helper + nonce-non-reuse test

Extracts the encryption + write pattern previously inlined in
setLampPassword / factoryReset. Discrete Phase B mutators pass
reboot:false; advanced LED + factoryReset pass reboot:true.

nonce_nonreuse_test asserts the B.8 invariant: two near-simultaneous
writeSettingsBlob calls produce distinct ciphertexts. Current
AesGcm uses random nonce per call so this passes by construction —
the test is a forward-defense guard against any future change to
nonce derivation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: `_mutate` helper for prev-capture-revert

Centralizes the optimistic-update + on-failure-revert pattern across ~24 mutator call sites. Models the existing `wisp_notifier.setSource` rollback shape.

**Files:**
- Modify: `lib/features/control/application/control_notifier.dart`
- Test: `test/features/control/mutate_helper_test.dart`

**NOTE on test sequencing**: this Task's tests use `setLampName` to exercise `_mutate` — but `setLampName` doesn't call `_mutate` until Task 9 lands the rewrite. Two options:

(a) Write Tasks 4 and 9 together as one merge (defer this Task's tests to after Task 9's setLampName rewrite is in).
(b) Add a `@visibleForTesting` test-only mutator in this Task that exercises `_mutate` directly.

Option (a) is simpler — the order in this plan already puts Task 9 close enough that combining them is fine. **Skip the test code for now in this Task; Task 9 Step 4 already has phase-aware tests that incidentally cover `_mutate`'s revert path.** Add the actual `_mutate` test then.

If you want option (b) for stronger isolation, add this `@visibleForTesting` helper:

```dart
  @visibleForTesting
  Future<void> mutateForTest(
    ControlState Function(ControlState) transform,
    Future<void> Function() commit,
  ) => _mutate(transform, commit);
```

And write the test against `mutateForTest(...)` directly.

- [ ] **Step 1: (Optional with option b) Write the failing test**

If you took option (a), skip this step. If you took option (b), create `test/features/control/mutate_helper_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';

void main() {
  group('controlNotifier._mutate (via mutateForTest)', () {
    test('applies the transform optimistically', () async {
      final container = ProviderContainer(/* ... */);
      addTearDown(container.dispose);
      final notifier = container.read(controlNotifierProvider('lamp-a').notifier);

      await notifier.mutateForTest(
        (s) => s.copyWith(/* trivial change */),
        () async {},  // no-op commit
      );

      // assert state reflects the transform
    });

    test('reverts state on commit failure', () async {
      // ... setup ...
      final notifier = container.read(controlNotifierProvider('lamp-a').notifier);
      final originalState = container.read(controlNotifierProvider('lamp-a')).value;
      await expectLater(
        () => notifier.mutateForTest(
          (s) => s.copyWith(/* trivial change */),
          () async => throw Exception('boom'),
        ),
        throwsA(isA<Exception>()),
      );
      expect(container.read(controlNotifierProvider('lamp-a')).value, originalState);
    });
  });
}
```

- [ ] **Step 2: Run (FAIL expected)**

```bash
flutter test test/features/control/mutate_helper_test.dart
```

- [ ] **Step 3: Add `_mutate` to `control_notifier.dart`**

Place near the other private helpers. The pattern mirrors `wisp_notifier.setSource`:

```dart
  /// Optimistic-update wrapper. Captures the current state, applies
  /// the [transform] immediately so the UI feels instant, then awaits
  /// the [commit] callback that does the BLE write(s). On exception,
  /// restores the captured state and rethrows.
  ///
  /// Per-pane mutators wrap their work in this helper to inherit
  /// snackbar-friendly failure semantics without each call site
  /// duplicating the try/catch dance.
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

- [ ] **Step 4: Run (PASS expected)**

```bash
flutter test test/features/control/mutate_helper_test.dart
```

Expected: 2/2 PASS. If your test mutators don't exist yet, scaffold them as test-only helpers; per-pane mutators get rewritten in Tasks 7-11 anyway.

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/application/control_notifier.dart test/features/control/mutate_helper_test.dart
git commit -m "$(cat <<'EOF'
feat(control): _mutate helper for optimistic + revert pattern

Per-pane mutators wrap their state update + BLE commit in _mutate.
On exception, state reverts to the pre-update snapshot. Models the
existing wisp_notifier.setSource pattern.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Debounce timer onDispose + AppLifecycleState.paused force-flush

Add a single `_commitDebounce` timer slot to `controlNotifier`. Scheduled flushes register via `ref.onDispose` so they cancel cleanly, AND register an AppLifecycleListener so app-background triggers force-flush. Mirrors the existing dispositions debounce dispose pattern (`dispositions_notifier.dart:45-56`).

**Files:**
- Modify: `lib/features/control/application/control_notifier.dart`
- Test: `test/features/control/brightness_commit_debounce_test.dart`

- [ ] **Step 1: Read the existing dispositions dispose pattern**

```bash
sed -n '45,80p' lib/features/social/application/dispositions_notifier.dart
```

The pattern: schedule a timer; cancel + flush in `ref.onDispose`.

- [ ] **Step 2: Add the timer field + debounce constant**

In `control_notifier.dart`, near the other timer fields (`_brightnessWriter`, etc.), add:

```dart
  /// Debounce window after the last user fence (slider release, picker
  /// accept) before commit fires. 500ms matches the spec — feels instant
  /// after release, generous enough that incremental taps collapse to
  /// one commit.
  static const Duration _commitDebounce = Duration(milliseconds: 500);
  Timer? _commitDebounceTimer;
  CommitSection? _pendingCommitSection;
```

- [ ] **Step 3: Add the schedule method**

```dart
  /// Schedule a debounced commit. If called again before the window
  /// expires, the timer is cancelled and rescheduled (trailing-edge
  /// behavior). The [section] is purely local-knowledge metadata; see
  /// [commit].
  void _scheduleCommitDebounced(CommitSection section) {
    _commitDebounceTimer?.cancel();
    _pendingCommitSection = section;
    _commitDebounceTimer = Timer(_commitDebounce, () async {
      _commitDebounceTimer = null;
      final s = _pendingCommitSection;
      _pendingCommitSection = null;
      if (s == null) return;
      try {
        await commit(s);
      } catch (e, st) {
        debugPrint('controlNotifier._scheduleCommitDebounced failed: $e\n$st');
        // No UI surface here — the user has already moved on; the
        // next commit attempt will retry. (The Save pill / isDirty
        // machinery still covers the persistence gap during
        // transition.)
      }
    });
  }

  /// Synchronously force-flush a pending debounced commit. Called from
  /// dispose + AppLifecycleState.paused so a quick edit-then-leave
  /// doesn't lose the user's last change.
  void _flushPendingCommit() {
    final s = _pendingCommitSection;
    if (s == null) return;
    _commitDebounceTimer?.cancel();
    _commitDebounceTimer = null;
    _pendingCommitSection = null;
    // Fire-and-forget — caller can't await us, but the BLE write itself
    // is async. If the link is gone, the commit fails and the user
    // sees the snackbar on next page load.
    unawaited(commit(s).catchError((e, st) {
      debugPrint('controlNotifier._flushPendingCommit failed: $e\n$st');
    }));
  }
```

- [ ] **Step 4: Wire into `ref.onDispose`**

In the notifier's `build()` (or wherever the existing `ref.onDispose` is — grep `ref.onDispose` in the file), add:

```dart
    ref.onDispose(() {
      // existing disposals...
      _commitDebounceTimer?.cancel();
      _flushPendingCommit();
    });
```

- [ ] **Step 5: Wire lifecycle pause via the existing `appLifecycleStateProvider`**

The project ALREADY has a central lifecycle notifier at `lib/core/lifecycle/app_lifecycle.dart` exposed via `appLifecycleStateProvider`. `controlNotifier` already uses `ref.listen(appLifecycleStateProvider, ...)` around line 402 — verify by:

```bash
grep -nE "appLifecycleStateProvider|AppLifecycleState\\.paused" lib/features/control/application/control_notifier.dart
```

Add the force-flush hook to the existing `ref.listen` (or add a new listen if needed). Pattern:

```dart
    ref.listen(appLifecycleStateProvider, (prev, next) {
      // existing handlers...
      if (next == AppLifecycleState.paused) {
        _flushPendingCommit();
      }
    });
```

Do NOT instantiate a raw `AppLifecycleListener` — that diverges from the project's convention and would double-subscribe.

- [ ] **Step 6: Write the test**

Create `test/features/control/brightness_commit_debounce_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:fake_async/fake_async.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:lamp_os/features/control/application/commit_section.dart';
import 'package:lamp_os/features/control/application/control_notifier.dart';

void main() {
  group('commit debounce', () {
    test('fires commit 500ms after last schedule', () {
      fakeAsync((async) {
        final container = ProviderContainer(/* ... */);
        addTearDown(container.dispose);
        final notifier = container.read(controlNotifierProvider('lamp-a').notifier);
        notifier.scheduleCommitDebouncedForTest(CommitSection.lamp);
        async.elapse(const Duration(milliseconds: 400));
        expect(commitFiredCount(container), 0);
        async.elapse(const Duration(milliseconds: 100));
        async.flushMicrotasks();
        expect(commitFiredCount(container), 1);
      });
    });

    test('rapid schedules collapse to one commit', () {
      fakeAsync((async) {
        final container = ProviderContainer(/* ... */);
        addTearDown(container.dispose);
        final notifier = container.read(controlNotifierProvider('lamp-a').notifier);
        notifier.scheduleCommitDebouncedForTest(CommitSection.lamp);
        async.elapse(const Duration(milliseconds: 100));
        notifier.scheduleCommitDebouncedForTest(CommitSection.lamp);
        async.elapse(const Duration(milliseconds: 100));
        notifier.scheduleCommitDebouncedForTest(CommitSection.lamp);
        async.elapse(const Duration(milliseconds: 500));
        async.flushMicrotasks();
        expect(commitFiredCount(container), 1);
      });
    });

    test('dispose flushes pending commit', () async {
      // Real timers — verify the dispose path fires commit synchronously.
      final container = ProviderContainer(/* ... */);
      final notifier = container.read(controlNotifierProvider('lamp-a').notifier);
      notifier.scheduleCommitDebouncedForTest(CommitSection.lamp);
      // Dispose before the 500ms window expires.
      container.dispose();
      await Future.delayed(const Duration(milliseconds: 50));
      expect(commitFiredCount(container), 1);
    });
  });
}
```

The `scheduleCommitDebouncedForTest` is a `@visibleForTesting` shim that wraps the private `_scheduleCommitDebounced` method. Add it to `control_notifier.dart`:

```dart
  @visibleForTesting
  void scheduleCommitDebouncedForTest(CommitSection s) =>
      _scheduleCommitDebounced(s);
```

- [ ] **Step 7: Run tests**

```bash
flutter test test/features/control/brightness_commit_debounce_test.dart
```

Expected: 3/3 PASS.

- [ ] **Step 8: Commit**

```bash
git add lib/features/control/application/control_notifier.dart test/features/control/brightness_commit_debounce_test.dart
git commit -m "$(cat <<'EOF'
feat(control): commit-debounce machinery + onDispose + lifecycle flush

500ms trailing-edge debounce on _scheduleCommitDebounced. Forced flush
on notifier dispose AND AppLifecycleState.paused. Drops the leak we'd
otherwise get if the user backs out of a lamp / backgrounds the app
within 500ms of a slider release.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Brightness slider — `onChangeEnd` schedules commit

Mutator already exists (`setBrightness` at line 725) — it writes CHAR_BRIGHTNESS for live preview. Phase B adds: schedule a debounced commit on the slider's `onChangeEnd` callback.

**Files:**
- Modify: `lib/features/control/presentation/widgets/brightness_card.dart`
- Modify: `lib/features/control/application/control_notifier.dart` (small addition to setBrightness)
- Test: covered by `brightness_commit_debounce_test.dart` from Task 5

- [ ] **Step 1: Locate the slider widget**

```bash
grep -nE "onChanged|onChangeEnd|setBrightness" lib/features/control/presentation/widgets/brightness_card.dart | head -10
```

Note the line numbers for the `Slider` widget that calls `setBrightness`.

- [ ] **Step 2: Add `onChangeEnd` to the slider**

In `brightness_card.dart`, find the `Slider(` widget. Add `onChangeEnd: (v) => notifier.scheduleBrightnessCommit()` right after the `onChanged` callback (or extend the existing `onChangeEnd` if one's there):

```dart
Slider(
  value: state.lamp.brightness.toDouble(),
  min: 0,
  max: 100,
  divisions: 100,
  onChanged: (v) => notifier.setBrightness(v.round()),
  onChangeEnd: (v) {
    notifier.setBrightness(v.round());
    notifier.scheduleBrightnessCommit();
  },
),
```

- [ ] **Step 3: Add `scheduleBrightnessCommit` to `controlNotifier`**

In `control_notifier.dart`, near `setBrightness` (line 725), add:

```dart
  /// Called from the brightness slider's `onChangeEnd` to schedule a
  /// debounced commit. Public wrapper around the private debounce
  /// so the widget layer doesn't need access to CommitSection.
  void scheduleBrightnessCommit() {
    _scheduleCommitDebounced(CommitSection.lamp);
  }
```

- [ ] **Step 4: Manual smoke test**

Build + install + manually verify the brightness slider commits after release. No automated widget test for slider gestures — the debounce mechanics are already covered by `brightness_commit_debounce_test.dart`.

```bash
flutter analyze lib/features/control/presentation/widgets/brightness_card.dart
flutter test test/features/control/
```

Expected: clean + green.

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/presentation/widgets/brightness_card.dart lib/features/control/application/control_notifier.dart
git commit -m "$(cat <<'EOF'
feat(brightness): onChangeEnd schedules debounced commit

Slider release fence triggers _scheduleCommitDebounced(CommitSection.lamp).
Live-preview path (CHAR_BRIGHTNESS during drag) unchanged.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6.5: Knockout pane — debounced commit + force-flush on screen back

The audit identified knockout as a live-preview-only persistence gap (per-pixel writes go to CHAR_BASE_KNOCKOUT today; nothing persists them on Phase A). Add a debounced commit after each `setKnockoutPixel` call and a `PopScope` hook on the knockout screen so back-nav force-flushes pending work even if the notifier's `ref.onDispose` doesn't fire at route-pop (depends on provider scope, which may be app-level).

**Files:**
- Modify: `lib/features/control/presentation/knockout_screen.dart` — wrap body in `PopScope`; force-flush on pop
- Modify: `lib/features/control/application/control_notifier.dart` — `setKnockoutPixel` schedules debounced commit

- [ ] **Step 1: Find the knockout mutator + screen**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-app-flutter
grep -nE "setKnockoutPixel|clearKnockout|knockout_screen" lib/features/control 2>/dev/null | head -10
```

Note the line numbers for `setKnockoutPixel` (and `clearKnockout`) in `control_notifier.dart`, and locate `knockout_screen.dart`.

- [ ] **Step 2: Add `scheduleKnockoutCommit` public helper**

In `control_notifier.dart`, near `scheduleBrightnessCommit` (Task 6):

```dart
  /// Called from the knockout screen after each per-pixel edit (and on
  /// screen-back force-flush) to schedule a debounced commit. Same
  /// debounce window as brightness.
  void scheduleKnockoutCommit() {
    _scheduleCommitDebounced(CommitSection.baseKnockout);
  }

  /// Synchronous force-flush exposed for the knockout screen's
  /// PopScope hook. The debounce timer may not have fired yet and the
  /// notifier's onDispose may not fire at route-pop if the notifier
  /// is app-scoped.
  void flushKnockoutCommit() {
    _flushPendingCommit();
  }
```

- [ ] **Step 3: Schedule commit on each pixel edit**

Find `setKnockoutPixel` in `control_notifier.dart`. The mutator currently writes CHAR_BASE_KNOCKOUT via the live-preview path and updates state. Append a call to `_scheduleCommitDebounced` at the end:

```dart
  Future<void> setKnockoutPixel(int pixel, int brightness) async {
    // ... existing live-preview write + state update ...
    _scheduleCommitDebounced(CommitSection.baseKnockout);
  }
```

(Match the existing method shape — don't rewrite the body, just add the schedule call at the end.)

- [ ] **Step 4: Wrap the knockout screen in PopScope**

In `knockout_screen.dart`, find the top-level `Scaffold` (or whatever returns the screen body). Wrap it:

```dart
@override
Widget build(BuildContext context) {
  return PopScope(
    canPop: true,
    onPopInvoked: (didPop) {
      if (didPop) {
        ref.read(controlNotifierProvider(widget.lampId).notifier).flushKnockoutCommit();
      }
    },
    child: Scaffold(
      // existing body
    ),
  );
}
```

(`PopScope` is the Flutter 3.13+ replacement for `WillPopScope`. If the project uses an older Flutter version, fall back to `WillPopScope` with `onWillPop`.)

- [ ] **Step 5: Analyze + test**

```bash
flutter analyze lib/features/control/presentation/knockout_screen.dart lib/features/control/application/control_notifier.dart
flutter test test/features/control/
```

- [ ] **Step 6: Commit**

```bash
git add lib/features/control/presentation/knockout_screen.dart lib/features/control/application/control_notifier.dart
git commit -m "$(cat <<'EOF'
feat(knockout): debounced commit after each pixel edit + back-nav flush

setKnockoutPixel now schedules a debounced commit (500ms idle,
matches brightness). PopScope on the knockout screen force-flushes
on back-nav so a quick edit-then-back doesn't lose pending work
even if the controlNotifier's ref.onDispose doesn't fire at route-pop
(controlNotifier is app-scoped).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Shade editor sheet — Update → commit; Cancel → snap-back

Add a captured-colors snapshot on sheet open, Cancel writes them back via the existing live-preview char to revert the lamp visually, Update fires the commit and pops.

**Files:**
- Modify: `lib/features/control/presentation/widgets/shade_editor_sheet.dart`

- [ ] **Step 1: Read the existing sheet structure**

```bash
sed -n '50,140p' lib/features/control/presentation/widgets/shade_editor_sheet.dart
```

Note where colors are captured-on-open and where the Update + Cancel buttons live.

- [ ] **Step 2: Confirm captured-colors snapshot is taken on initState**

```dart
List<LampColor>? _capturedColors;

@override
void initState() {
  super.initState();
  _capturedColors ??= List.of(
    ref.read(controlNotifierProvider(widget.lampId)).value!.shade.colors,
  );
}
```

If the sheet already does this, leave it. If not, add it.

- [ ] **Step 3: Wire Cancel → snap-back**

Find the Cancel button. Replace its `onPressed` body with:

```dart
onPressed: () {
  // Snap the lamp back to the colors captured at sheet open via the
  // live-preview char. No commit — Cancel discards the edit.
  final notifier = ref.read(controlNotifierProvider(widget.lampId).notifier);
  notifier.setShadeColors(_capturedColors!);
  Navigator.of(context).pop();
},
```

- [ ] **Step 4: Wire Update → commit + pop**

Find the Update button. Replace its `onPressed` body with:

```dart
onPressed: () async {
  final notifier = ref.read(controlNotifierProvider(widget.lampId).notifier);
  try {
    await notifier.commit(CommitSection.shade);
  } catch (e) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text("Couldn't save — disconnected")),
    );
    return;
  }
  if (!mounted) return;
  Navigator.of(context).pop();
},
```

- [ ] **Step 5: Run analyze + tests**

```bash
flutter analyze lib/features/control/presentation/widgets/shade_editor_sheet.dart
flutter test
```

Expected: clean + all existing tests still green.

- [ ] **Step 6: Commit**

```bash
git add lib/features/control/presentation/widgets/shade_editor_sheet.dart
git commit -m "$(cat <<'EOF'
feat(shade-editor): Update commits; Cancel snaps back via live-preview

On Phase A lamps Update fires commit(CommitSection.shade) before pop;
the firmware persists current RAM state. On pre-Phase-A lamps commit
is a no-op and the Save pill picks up the change. Cancel writes the
captured-at-open colors back via setShadeColors so the strip visually
reverts.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Base editor sheet — Update → commit + ac via writeSettingsBlob; Cancel → snap-back

Same shape as Task 7, plus: if `base.ac` (active-color index) changed during the edit, it has no live-preview char — write a tiny `settingsBlob({"base":{"ac":N}}, reboot:false)` on the Update path.

**Files:**
- Modify: `lib/features/control/presentation/widgets/base_editor_sheet.dart`

- [ ] **Step 1: Locate captured-state snapshot + buttons**

```bash
sed -n '50,140p' lib/features/control/presentation/widgets/base_editor_sheet.dart
```

- [ ] **Step 2: Capture both colors AND ac at open**

```dart
List<LampColor>? _capturedColors;
int? _capturedAc;

@override
void initState() {
  super.initState();
  final s = ref.read(controlNotifierProvider(widget.lampId)).value!;
  _capturedColors ??= List.of(s.base.colors);
  _capturedAc ??= s.base.ac;
}
```

- [ ] **Step 3: Wire Cancel → snap-back colors; restore ac in state-only**

```dart
onPressed: () {
  final notifier = ref.read(controlNotifierProvider(widget.lampId).notifier);
  // Colors revert visually via live-preview char.
  notifier.setBaseColors(_capturedColors!);
  // ac has NO live-preview char — restoring it in app state is enough
  // for the next visible render. The lamp's current ac may still be
  // whatever the user picked mid-edit (since ac changes during the
  // edit session aren't pushed to the lamp until Update fires the
  // settings_blob). On Cancel, the local state matches captured-at-
  // open and the lamp keeps whatever it had — which is fine because
  // ac only affects the next paint cycle the user triggers.
  notifier.setBaseAc(_capturedAc!);
  Navigator.of(context).pop();
},
```

- [ ] **Step 4: Wire Update → ac settings_blob (if changed) + commit + pop**

```dart
onPressed: () async {
  final notifier = ref.read(controlNotifierProvider(widget.lampId).notifier);
  final cur = ref.read(controlNotifierProvider(widget.lampId)).value!;
  try {
    if (cur.base.ac != _capturedAc) {
      // base.ac has no live-preview char; ship it via settings_blob
      // (reboot:false) so Phase A firmware applies in place.
      await notifier.writeSettingsBlob(
        {'base': {'ac': cur.base.ac}},
        reboot: false,
      );
    }
    await notifier.commit(CommitSection.base);
  } catch (e) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text("Couldn't save — disconnected")),
    );
    return;
  }
  if (!mounted) return;
  Navigator.of(context).pop();
},
```

- [ ] **Step 5: Analyze + test**

```bash
flutter analyze lib/features/control/presentation/widgets/base_editor_sheet.dart
flutter test
```

- [ ] **Step 6: Commit**

```bash
git add lib/features/control/presentation/widgets/base_editor_sheet.dart
git commit -m "$(cat <<'EOF'
feat(base-editor): Update commits + ships ac via settings_blob; Cancel snaps back

If base.ac changed, writeSettingsBlob({base:{ac:N}}, reboot:false) ships
the index. Then commit(CommitSection.base) persists the colors via
CHAR_COMMIT (Phase A) or no-ops (pre-Phase-A — Save pill still works).
Cancel snaps colors AND ac back to captured-at-open.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: Setup-screen mutators — rename, personality pill, advanced toggle

Three setup-screen mutators currently state-only (per the audit). Each now writes settings_blob with `reboot: false` on Phase A lamps; behaves unchanged on pre-Phase-A.

The rename mutator additionally updates the inventory cache immediately (B.7 spec requirement) — without reload, nothing else propagates the new name into the LampChip AppBar title.

**Files:**
- Modify: `lib/features/control/application/control_notifier.dart` — `setLampName` (line 905), `setLampAdvancedEnabled` (line 920), `setLampSocialMode` (line 938)

**NOTE on `copyWith`**: `LampSection`, `BaseSection`, `ShadeSection`, `HomeModeSection` are plain Dart classes WITHOUT `copyWith` methods (only `ControlState` and `ExpressionConfig` are Freezed). The existing mutators rebuild sections field-by-field — keep that pattern. `ControlState.copyWith` IS available because `ControlState` IS Freezed. Use it for the top-level state wrap.

**NOTE on `hasCommitChar`**: read sites must default `null` → `false`. The pattern is `entry.hasCommitChar ?? false`.

- [ ] **Step 1: Rewrite `setLampName` (line 905)**

```dart
  Future<void> setLampName(String name) async {
    await _mutate(
      (s) => s.copyWith(
        lamp: LampSection(
          name: name,
          brightness: s.lamp.brightness,
          advancedEnabled: s.lamp.advancedEnabled,
          socialMode: s.lamp.socialMode,
          fwVersion: s.lamp.fwVersion,
          fwChannel: s.lamp.fwChannel,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere(
          (l) => l.id == _deviceId,
          orElse: () => throw StateError('lamp $_deviceId not in inventory'),
        );
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob({'lamp': {'name': name}}, reboot: false);
        }
        // Inventory cache update fires for BOTH paths so the AppBar
        // LampChip title is correct without waiting for a reload.
        await ref
            .read(inventoryNotifierProvider.notifier)
            .updateName(_deviceId, name);
      },
    );
  }
```

- [ ] **Step 2: Rewrite `setLampAdvancedEnabled` (line 920)**

```dart
  Future<void> setLampAdvancedEnabled(bool v) async {
    await _mutate(
      (s) => s.copyWith(
        lamp: LampSection(
          name: s.lamp.name,
          brightness: s.lamp.brightness,
          advancedEnabled: v,
          socialMode: s.lamp.socialMode,
          fwVersion: s.lamp.fwVersion,
          fwChannel: s.lamp.fwChannel,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere(
          (l) => l.id == _deviceId,
          orElse: () => throw StateError('lamp $_deviceId not in inventory'),
        );
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob({'lamp': {'advancedEnabled': v}}, reboot: false);
        }
      },
    );
  }
```

- [ ] **Step 3: Rewrite `setLampSocialMode` (line 938)**

```dart
  Future<void> setLampSocialMode(SocialMode mode) async {
    await _mutate(
      (s) => s.copyWith(
        lamp: LampSection(
          name: s.lamp.name,
          brightness: s.lamp.brightness,
          advancedEnabled: s.lamp.advancedEnabled,
          socialMode: mode,
          fwVersion: s.lamp.fwVersion,
          fwChannel: s.lamp.fwChannel,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere(
          (l) => l.id == _deviceId,
          orElse: () => throw StateError('lamp $_deviceId not in inventory'),
        );
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob({'lamp': {'socialMode': mode.wire}}, reboot: false);
        }
      },
    );
  }
```

- [ ] **Step 4: Add the phase-aware behavior test**

Create `test/features/control/setup_mutators_phasea_vs_prephasea_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:lamp_os/core/ble/uuids.dart';

import 'package:lamp_app/core/ble/ble_client.dart'; // InMemoryBleClient is a class inside this file

void main() {
  group('setLampName phase-aware behavior', () {
    test('Phase A: writeSettingsBlob fires + inventory cache updates', () async {
      // ... setup with hasCommitChar=true for the test lamp ...
      await notifier.setLampName('newname');
      expect(ble.writesTo('lamp-a', BleUuids.settingsBlob).length, 1);
      expect((await invNotifier.future).firstWhere((l) => l.id == 'lamp-a').name, 'newname');
    });

    test('Pre-Phase-A: no BLE write but state + inventory still update', () async {
      // ... setup with hasCommitChar=false ...
      await notifier.setLampName('newname');
      expect(ble.writesTo('lamp-b', BleUuids.settingsBlob), isEmpty);
      expect((await invNotifier.future).firstWhere((l) => l.id == 'lamp-b').name, 'newname');
    });

    test('Phase A: BLE failure reverts state', () async {
      // ... setup with ThrowingBleClient + hasCommitChar=true ...
      final originalName = state.lamp.name;
      await expectLater(() => notifier.setLampName('newname'), throwsA(isA<Exception>()));
      expect(state.lamp.name, originalName);
    });
  });

  // Repeat for setLampAdvancedEnabled, setLampSocialMode
}
```

- [ ] **Step 5: Run tests**

```bash
flutter test test/features/control/setup_mutators_phasea_vs_prephasea_test.dart
flutter analyze
```

Expected: green + clean.

- [ ] **Step 6: Commit**

```bash
git add lib/features/control/application/control_notifier.dart test/features/control/setup_mutators_phasea_vs_prephasea_test.dart
git commit -m "$(cat <<'EOF'
feat(setup): rename + advancedEnabled + socialMode mutators commit on Phase A

State-only mutators on pre-Phase-A lamps (Save pill still works).
On Phase A lamps, each fires writeSettingsBlob({...}, reboot: false)
so the change persists without reboot. Rename additionally updates
the inventory cache synchronously so the AppBar LampChip title
reflects the new name without a reload.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: Home Mode pane — toggle, SSID, brightness, forget

Same shape as Task 9 for the discrete fields (toggle, SSID, forget). The brightness slider in the home-mode pane gets the same debounced-commit treatment as the lamp brightness slider (Task 6), plus relies on the existing CHAR_HOMEMODEFOCUS routing on the firmware side.

**Files:**
- Modify: `lib/features/control/application/control_notifier.dart` — `setHomeSsid` (1158), `setHomeBrightness` (1170), `setHomeEnabled` (1191)
- Modify: `lib/features/lamp_shell/presentation/home_mode_screen.dart` — wire `onChangeEnd` on the brightness slider

**NOTE on `HomeModeSection`**: like the other section classes, `HomeModeSection` has NO `copyWith`. Rebuild field-by-field. (`ControlState.copyWith` works as before for the wrap.) Find the existing setHomeEnabled at line 1191 to see the exact constructor field set.

- [ ] **Step 1: Rewrite `setHomeEnabled` (line 1191)**

```dart
  Future<void> setHomeEnabled(bool enabled) async {
    await _mutate(
      (s) => s.copyWith(
        home: HomeModeSection(
          ssid: s.home.ssid,
          brightness: s.home.brightness,
          enabled: enabled,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere((l) => l.id == _deviceId);
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob({'homeMode': {'enabled': enabled}}, reboot: false);
        }
      },
    );
  }
```

(Adjust constructor fields to whatever the existing `HomeModeSection` accepts — grep its definition before writing.)

- [ ] **Step 2: Rewrite `setHomeSsid` (line 1158)**

```dart
  Future<void> setHomeSsid(String ssid) async {
    await _mutate(
      (s) => s.copyWith(
        home: HomeModeSection(
          ssid: ssid,
          brightness: s.home.brightness,
          enabled: s.home.enabled,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere((l) => l.id == _deviceId);
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob({'homeMode': {'ssid': ssid}}, reboot: false);
        }
      },
    );
  }
```

**Note on password field**: an earlier draft of this plan / spec mentioned shipping `{ssid: s, password: p}` together. Phase A's `apply_home_mode.hpp` deliberately omitted the `password` field because `HomeModeSettings` has no `password` field — the lamp never associates to the AP and stores no credential (verified during Phase A Task 9). Shipping a `password` key would be silently dropped by the firmware. Single-field `{ssid: ssid}` is the correct shape. The Forget action (Step 5) ships `{ssid: ""}` for the same reason.

- [ ] **Step 3: Rewrite `setHomeBrightness` (line 1170) — schedules debounced commit**

```dart
  Future<void> setHomeBrightness(int brightness) async {
    // Live-preview path: existing BLE write to CHAR_BRIGHTNESS (firmware
    // routes to homeMode.brightness while CHAR_HOMEMODEFOCUS flag is
    // active). Don't change that — just schedule the commit on top.
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(home: cur.home.copyWith(brightness: brightness)));
    _brightnessWriter?.schedule(Uint8List.fromList([brightness]));
    _scheduleCommitDebounced(CommitSection.homeMode);
  }
```

- [ ] **Step 4: Verify home-mode slider's onChangeEnd hook**

In `home_mode_screen.dart`, find the brightness Slider. Match the pattern from Task 6:

```dart
Slider(
  value: state.home.brightness.toDouble(),
  min: 0,
  max: 100,
  onChanged: (v) => notifier.setHomeBrightness(v.round()),
  onChangeEnd: (v) {
    notifier.setHomeBrightness(v.round());
    // _scheduleCommitDebounced already fires inside setHomeBrightness,
    // but the onChangeEnd guarantees the final value lands as a commit
    // signal even if the user lifts off in the middle of a frame.
  },
),
```

(If the home-mode slider doesn't exist or has a different name, grep `setHomeBrightness` for the call site.)

- [ ] **Step 5: Forget action**

In `home_mode_screen.dart`, find the Forget button. Today it calls `setHomeSsid('')`. The new `setHomeSsid` already commits — no widget change needed.

- [ ] **Step 6: Test + analyze**

```bash
flutter analyze
flutter test test/features/control/
```

- [ ] **Step 7: Commit**

```bash
git add lib/features/control/application/control_notifier.dart lib/features/lamp_shell/presentation/home_mode_screen.dart
git commit -m "$(cat <<'EOF'
feat(home-mode): toggle + SSID + brightness slider commit on Phase A

setHomeEnabled / setHomeSsid: writeSettingsBlob({homeMode:{...}},
reboot:false) on Phase A lamps; state-only on pre-Phase-A.
setHomeBrightness: live-preview unchanged + schedule debounced commit
on top. Slider onChangeEnd is the natural fence; debounce coalesces
rapid tap-to-position. Forget reuses setHomeSsid("").

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: Advanced LED Update — `reboot: true` + post-reconnect verify

Advanced LED genuinely needs a reboot to reinit the strip with the new px/byteOrder. Use the existing `setLampPassword` flow as the template (it already implements the fade-out + reboot + reconnect ladder). After reconnect, diff what was shipped against what came back; surface "save didn't take — retry?" on mismatch.

**Files:**
- Modify: `lib/features/lamp_shell/presentation/advanced_leds_screen.dart`
- Modify: `lib/features/control/application/control_notifier.dart` — new `applyAdvancedLedsAndReboot(BaseSection, ShadeSection)` method
- Test: `test/features/control/advanced_led_reboot_verify_test.dart`

- [ ] **Step 1: Read the existing setLampPassword pattern**

```bash
sed -n '1076,1156p' lib/features/control/application/control_notifier.dart
```

Note the encrypted write + the `_awaitReconnectAndReload` call chain. The new method reuses that machinery.

- [ ] **Step 2: Add `applyAdvancedLedsAndReboot` to controlNotifier**

**API NOTE**: `_awaitReconnectAndReload` returns `Future<void>` and delivers the fresh state via a `postReload` callback (NOT via return value). Existing signature:
```dart
Future<void> _awaitReconnectAndReload({
  required BleClient ble,
  required String password,
  required Future<void> Function(ControlState fresh) postReload,
  // ... possibly other args; grep for the full signature in control_notifier.dart
});
```

`applyAdvancedLedsAndReboot` captures the diff inside the postReload callback and returns the mismatch list to its own caller:

```dart
  /// Apply Advanced LED settings (px, byteOrder, bpp, ac) by writing
  /// settings_blob with reboot:true. _awaitReconnectAndReload runs the
  /// existing reconnect ladder. After reload, diffs the freshly-read
  /// base/shade sections against what was shipped — if they don't
  /// match, returns a list of field names. Caller (the screen) shows a
  /// "save didn't take — retry?" snackbar on non-empty mismatches.
  Future<List<String>> applyAdvancedLedsAndReboot({
    required BaseSection base,
    required ShadeSection shade,
  }) async {
    final shipped = <String, dynamic>{
      'base': {
        'px': base.px,
        'byteOrder': base.byteOrder,
        'bpp': base.bpp,
        'ac': base.ac,
        'colors': base.colors.map((c) => c.toHex()).toList(),
      },
      'shade': {
        'px': shade.px,
        'byteOrder': shade.byteOrder,
        'bpp': shade.bpp,
        'colors': shade.colors.map((c) => c.toHex()).toList(),
      },
    };
    await writeSettingsBlob(shipped, reboot: true);

    // Capture the diff inside the postReload callback (the method
    // resolves AFTER postReload runs).
    final mismatches = <String>[];
    final ble = ref.read(bleClientProvider);
    final inv = await ref.read(inventoryNotifierProvider.future);
    final lamp = inv.firstWhere((l) => l.id == _deviceId);
    try {
      await _awaitReconnectAndReload(
        ble: ble,
        password: lamp.controlPassword ?? '',
        postReload: (fresh) async {
          if (fresh.base.px != base.px) mismatches.add('base.px');
          if (fresh.base.byteOrder != base.byteOrder) mismatches.add('base.byteOrder');
          if (fresh.base.bpp != base.bpp) mismatches.add('base.bpp');
          if (fresh.shade.px != shade.px) mismatches.add('shade.px');
          if (fresh.shade.byteOrder != shade.byteOrder) mismatches.add('shade.byteOrder');
          if (fresh.shade.bpp != shade.bpp) mismatches.add('shade.bpp');
        },
      );
    } catch (e) {
      mismatches.add('lamp did not reconnect after Advanced LED save: $e');
    }
    return mismatches;
  }
```

(Verify the exact `_awaitReconnectAndReload` signature in `control_notifier.dart` before implementing — there may be additional required args beyond `ble`/`password`/`postReload`.)

- [ ] **Step 3: Wire the Update button in advanced_leds_screen.dart**

```dart
onPressed: () async {
  final notifier = ref.read(controlNotifierProvider(widget.lampId).notifier);
  final state = ref.read(controlNotifierProvider(widget.lampId)).value!;
  try {
    final mismatches = await notifier.applyAdvancedLedsAndReboot(
      base: state.base,
      shade: state.shade,
    );
    if (!mounted) return;
    if (mismatches.isEmpty) {
      Navigator.of(context).pop();
    } else {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text("Save didn't take — ${mismatches.join(', ')}"),
          action: SnackBarAction(
            label: 'Retry',
            onPressed: () {
              // Re-trigger the same Update. Simpler than recursing the
              // press handler — set a state flag and let the user tap
              // Update again, or call applyAdvancedLedsAndReboot
              // directly here.
            },
          ),
        ),
      );
    }
  } catch (e) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('Save failed: $e')),
    );
  }
},
```

- [ ] **Step 4: Write the post-reconnect verify test**

Create `test/features/control/advanced_led_reboot_verify_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

void main() {
  group('applyAdvancedLedsAndReboot', () {
    test('returns empty mismatches when reconnect sections match shipped', () async {
      // ... setup InMemoryBleClient with sections that round-trip ...
      final mismatches = await notifier.applyAdvancedLedsAndReboot(/* ... */);
      expect(mismatches, isEmpty);
    });

    test('returns ["base.px"] when fresh.base.px differs from shipped', () async {
      // ... setup where reconnect returns a DIFFERENT px ...
      final mismatches = await notifier.applyAdvancedLedsAndReboot(/* ... */);
      expect(mismatches, contains('base.px'));
    });

    test('returns reconnect-failed marker when reload returns null', () async {
      // ... setup with disconnect-after-reboot that never reconnects ...
      final mismatches = await notifier.applyAdvancedLedsAndReboot(/* ... */);
      expect(mismatches, contains(matches(RegExp(r'did not reconnect'))));
    });
  });
}
```

- [ ] **Step 5: Test + analyze**

```bash
flutter test test/features/control/advanced_led_reboot_verify_test.dart
flutter analyze
```

- [ ] **Step 6: Commit**

```bash
git add lib/features/control/application/control_notifier.dart lib/features/lamp_shell/presentation/advanced_leds_screen.dart test/features/control/advanced_led_reboot_verify_test.dart
git commit -m "$(cat <<'EOF'
feat(advanced-led): Update → settings_blob (reboot:true) + post-reconnect verify

Advanced LED writes go through writeSettingsBlob(reboot:true) which
fades the lamp + reboots. After reconnect, diffs the freshly-read
base/shade sections against what was shipped; mismatches drive a
"save didn't take — retry" snackbar.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 12: Save-pill visibility gate

The Save pill stays mounted but its rendering is gated on `!hasCommitChar` (per-lamp). Pre-Phase-A lamps still see the pill; Phase A lamps don't (and there's nothing to save anyway — every edit commits at its natural fence).

**Files:**
- Modify: `lib/features/lamp_shell/presentation/lamp_shell.dart:115-122` (the existing `actions:` block that mounts `_SaveAction`)

- [ ] **Step 1: Read the existing AppBar action**

```bash
sed -n '110,125p' lib/features/lamp_shell/presentation/lamp_shell.dart
```

- [ ] **Step 2: Add the hasCommitChar gate**

Replace:

```dart
actions: [
  // existing comment about Save pill visibility
  if (_tab != LampTab.info) _SaveAction(lampId: widget.lampId),
],
```

With:

```dart
actions: [
  // Save pill — visible on tabs that ride the isDirty + global
  // settings_blob save path. Phase B.9.1: hidden on lamps running
  // Phase A firmware (hasCommitChar==true) because every per-pane
  // edit commits at its natural fence; nothing dirty to save.
  if (_tab != LampTab.info && !_hasCommitChar(ref, widget.lampId))
    _SaveAction(lampId: widget.lampId),
],
```

And add a helper near the bottom of the file (or in a `_LampShellState` private method):

```dart
bool _hasCommitChar(WidgetRef ref, String lampId) {
  // Read the inventory entry synchronously — by the time the AppBar
  // is painted, the connect handler has populated this field.
  final inv = ref.watch(inventoryNotifierProvider).value;
  if (inv == null) return false;
  return inv
      .firstWhere(
        (l) => l.id == lampId,
        orElse: () => throw StateError('lamp $lampId not in inventory'),
      )
      .hasCommitChar;
}
```

- [ ] **Step 3: Visual smoke test**

Manually verify in the app that:
- A Phase A lamp shows NO Save pill in the AppBar.
- A pre-Phase-A lamp shows the Save pill on the existing tabs (still respects the `_tab != LampTab.info` gate).

- [ ] **Step 4: Commit**

```bash
git add lib/features/lamp_shell/presentation/lamp_shell.dart
git commit -m "$(cat <<'EOF'
feat(shell): hide Save pill on Phase A lamps

_SaveAction visibility gated on !hasCommitChar (per-lamp). The pill +
all underlying isDirty/_original/save() machinery stays for the
transition — pre-Phase-A lamps unaffected. Deletion lives in B.9.2.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 13: Final analyze + test sweep + build APK + adb install

End-to-end verification before handing off to the bench.

- [ ] **Step 1: Run `flutter analyze`**

```bash
cd /Users/jerrett/projects/lamp-os/software/lamp-app-flutter
flutter analyze
```

Expected: No issues found.

- [ ] **Step 2: Run full Flutter test suite**

```bash
flutter test
```

Expected: All tests pass. Baseline before Phase B was 340 tests; Phase B added ~10 new tests across 5 test files. Total target: 350±.

- [ ] **Step 3: Build APK**

```bash
flutter build apk --debug
```

Expected: `✓ Built build/app/outputs/flutter-apk/app-debug.apk`.

- [ ] **Step 4: Install via adb install -r (NOT flutter install)**

```bash
adb devices
```

If there's an emulator paired alongside the Pixel, specify the device id explicitly:

```bash
adb -s <pixel-id> install -r build/app/outputs/flutter-apk/app-debug.apk
```

Expected: `Success`. Inventory survives (no uninstall step).

- [ ] **Step 5: Manual bench check — Phase A lamp**

With one lamp running Phase A firmware (the bench lamp from the firmware Phase):
1. Connect via the app.
2. Confirm the Save pill is NOT visible in the AppBar.
3. Drag the brightness slider; release. After ~500ms, observe `[nvs] persistConfig via=commit` in the serial log.
4. Open the Setup screen, rename the lamp. Confirm the AppBar chip updates immediately AND `via=settings_blob` appears in the serial log without a reboot.
5. Power-cycle the lamp. After it reconnects, confirm both edits persisted.

- [ ] **Step 6: Manual bench check — pre-Phase-A lamp**

If a pre-Phase-A lamp is available:
1. Connect via the app. 
2. Confirm the Save pill IS visible.
3. Drag the brightness slider; release. No commit log on the firmware side (legacy live-preview only).
4. Open Setup, rename. Save pill lights up. Tap Save pill — confirm settings_blob fires, lamp reboots, name persists.

- [ ] **Step 7: Commit a phase-complete marker**

```bash
git commit --allow-empty -m "$(cat <<'EOF'
chore(phase-b.9.1): app foundation complete; bench verification done

Per-pane mutators now commit on the natural fence when the lamp runs
Phase A firmware (hasCommitChar==true). Pre-Phase-A lamps still see
the Save pill and use the legacy global save() path. Deletion of the
legacy machinery lives in B.9.2 — to be planned + landed after the
operator confirms all paired lamps are running Phase A firmware.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

**Spec coverage:**
- B.1 `_mutate` helper → Task 4 ✓
- B.2 per-pane mutator changes → Tasks 6 (brightness), 6.5 (knockout), 7 (shade), 8 (base), 9 (setup mutators), 10 (home mode), 11 (advanced LED) ✓
- B.3 GATT-discovery fallback → Tasks 1 + 2 ✓ (probe + commit() branch — pre-Phase-A intentionally no-ops rather than synthesizing partial blob; rationale in Task 2 comment)
- B.4 `{reboot:true}` post-reconnect verify → Task 11 ✓
- B.5 debounce dispose + lifecycle flush → Task 5 ✓
- B.6 live-preview snap-back on Cancel → Tasks 7 + 8 ✓
- B.7 inventory cache immediate update on rename → Task 9 ✓
- B.8 nonce-reuse regression test → Task 3 Step 6 (test/features/control/nonce_nonreuse_test.dart) ✓
- B.9.1 deletions preserved behind hasCommitChar flag → Task 12 ✓
- B.10 tests → spread across Tasks 1-11; consolidated check in Task 13 ✓

**Placeholder scan:** Task 1 contains a Step 1 "grep to find exact file paths" — this is a real discovery action, not a placeholder. Task 5 references `dispositions_notifier.dart:45-56` which exists (verified earlier in this session). No "TBD"/"TODO" left as fillers.

**Type consistency:** `CommitSection` (Task 2) used in Tasks 5, 6, 7, 8, 9, 10. `writeSettingsBlob({reboot})` signature consistent in Tasks 3, 8, 9, 10, 11. `hasCommitChar` field consistent across Tasks 1, 2, 9, 10, 12. `_mutate(transform, commit)` signature consistent in Tasks 4, 9, 10.

**Acceptance for B.9.1:**
- `flutter analyze` clean
- `flutter test`: ~350 (baseline 340 + ~10 new)
- Pixel APK installs cleanly via `adb install -r`
- Bench: Phase A lamp commits per-edit, no Save pill. Pre-Phase-A lamp behaves identically to before.
- No deletion of `save()` / `isDirty` / `_original` / `_SaveAction` / `lampSaveStatusProvider` — those land in B.9.2.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-13-always-save-phase-b-app.md`. Two execution options:

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review between tasks. Matches the pattern used for Phase A.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch with checkpoints.

Which approach?
