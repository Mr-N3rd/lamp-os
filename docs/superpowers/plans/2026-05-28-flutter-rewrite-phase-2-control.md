# Flutter Rewrite — Phase 2 (Control screen) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Replace the Control tab placeholder with a working surface that, after the user opens a per-lamp shell, connects + auths + reads the lamp's brightness / shade / base from its per-section characteristics and lets the user change them: a brightness slider, a single shade color swatch (tap → color picker), and a compact base card (tap → bottom-sheet editor with up to 5 stops + active color).

**Architecture:** A per-device `controlNotifierProvider.family(deviceId)` (Riverpod `AsyncNotifier`) drives the screen. On mount it connects, writes `CHAR_AUTH` with the lamp's password from inventory, then reads `CHAR_LAMP_SECTION` + `CHAR_BASE_SECTION` + `CHAR_SHADE_SECTION` and parses them into a Freezed `ControlState`. UI cards read from the notifier and dispatch through it; the notifier owns a `WriteCoalescer` per characteristic so slider-drag writes debounce to ~30 ms gaps. The shared `ColorPickerSheet` is opened from both shade and base flows and emits a `LampColor` (RGB + W byte) back to the caller. No `CHAR_STATE_NOTIFY` live-refresh in this phase — pulled into a Phase 2.x follow-up so this slice stays scoped.

**Tech Stack:** existing Riverpod 4 + Freezed 3 + flutter_blue_plus 2.3, plus `flutter_colorpicker: ^1.1.0` for the HSV picker (deferring a hand-rolled hue ring to a later polish pass — see spec §15 open question 1).

**Spec:** [docs/superpowers/specs/2026-05-28-flutter-rewrite-design.md](../specs/2026-05-28-flutter-rewrite-design.md) §6, §7 (write coalescing + per-section reads), §8.2 (control screen), §8.3 (base editor sheet), §9 (color picker row).

**Prerequisite:** Phase 1b (AddLamp wizard) is complete and on real hardware. `InMemoryBleClient`, `bleClientProvider`, and the lamp shell with the Control tab placeholder all exist.

**Phases after this one:** Phase 2.x — `CHAR_STATE_NOTIFY` subscription so external client edits refresh the screen; Phase 3 — knockout editor; Phase 4 — expressions; Phase 5 — setup screen (incl. WiFi + MQTT).

**Out of scope for this phase** (intentional, despite being on spec §8.2 / §6):
- `WriteOrForward` / remote-target banner — single active lamp ("self" only). Multi-lamp + mesh forwarding deferred to a later phase.
- Knockout summary card on the control screen — that's the leading edge of Phase 3.
- `CHAR_STATE_NOTIFY` subscription — Phase 2.x.

---

## File Structure (this plan creates / modifies)

```
software/lamp-app-flutter/lib/
├── core/
│   └── ble/
│       └── write_coalescer.dart                    # T1 — debouncer
├── features/
│   ├── inventory/
│   │   └── domain/
│   │       └── inventory_lamp.dart                 # T2 — add controlPassword field
│   └── control/
│       ├── domain/
│       │   ├── lamp_color.dart                     # T3 — RGBW + hex codec
│       │   └── sections.dart                       # T4 — LampSection, BaseSection, ShadeSection
│       ├── application/
│       │   ├── auth_client.dart                    # T5 — writes CHAR_AUTH
│       │   ├── control_state.dart                  # T6 — Freezed combined state
│       │   └── control_notifier.dart               # T7 — async family, owns coalescers
│       └── presentation/
│           ├── control_screen.dart                 # T13 — assembles cards
│           └── widgets/
│               ├── color_picker_sheet.dart         # T8 — shared bottom sheet
│               ├── brightness_card.dart            # T9
│               ├── shade_card.dart                 # T10
│               ├── base_card.dart                  # T11 — compact preview
│               └── base_editor_sheet.dart          # T12 — full editor
└── features/onboarding/application/
    └── add_lamp_notifier.dart                      # T2 — pass password into inventory

software/lamp-app-flutter/test/
├── core/ble/write_coalescer_test.dart              # T1
├── features/control/
│   ├── domain/lamp_color_test.dart                 # T3
│   ├── domain/sections_test.dart                   # T4
│   └── application/
│       ├── auth_client_test.dart                   # T5
│       └── control_notifier_test.dart              # T7
└── features/control/presentation/widgets/
    ├── brightness_card_test.dart                   # T9
    ├── shade_card_test.dart                        # T10
    ├── base_card_test.dart                         # T11
    └── base_editor_sheet_test.dart                 # T12
```

The widget tests are smoke tests (pumpWidget + tap + verify a method was called on a fake notifier). No golden tests in this phase. Real-hardware verification lives in T14.

---

## Task 1: WriteCoalescer

A debouncer that owns a single `Future<void> Function(Uint8List)` write function and a debounce duration. `schedule(payload)` replaces any pending payload; the function fires after the debounce window. `flush()` forces an immediate send. `dispose()` cancels pending work.

**Files:**
- Create: `software/lamp-app-flutter/lib/core/ble/write_coalescer.dart`
- Create: `software/lamp-app-flutter/test/core/ble/write_coalescer_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/core/ble/write_coalescer_test.dart`:

```dart
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/write_coalescer.dart';

void main() {
  test('debounces back-to-back schedules into a single write', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(milliseconds: 20),
    );
    c.schedule(Uint8List.fromList([1]));
    c.schedule(Uint8List.fromList([2]));
    c.schedule(Uint8List.fromList([3]));
    await Future<void>.delayed(const Duration(milliseconds: 40));
    expect(writes, [Uint8List.fromList([3])]);
  });

  test('flush() forces immediate send and cancels pending timer', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(seconds: 10),
    );
    c.schedule(Uint8List.fromList([1]));
    await c.flush();
    expect(writes, [Uint8List.fromList([1])]);
  });

  test('dispose() cancels pending work', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(milliseconds: 20),
    );
    c.schedule(Uint8List.fromList([1]));
    c.dispose();
    await Future<void>.delayed(const Duration(milliseconds: 40));
    expect(writes, isEmpty);
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/core/ble/write_coalescer_test.dart
```
Expected: compile error (`WriteCoalescer` undefined).

- [ ] **Step 3: Implement WriteCoalescer**

Create `lib/core/ble/write_coalescer.dart`:

```dart
import 'dart:async';
import 'dart:typed_data';

/// Coalesces a stream of `schedule(payload)` calls into a single trailing
/// write after [debounce] has elapsed since the last call. Used to keep
/// slider drags from firing dozens of BLE writes per second.
class WriteCoalescer {
  WriteCoalescer({required this.onWrite, required this.debounce});

  final Future<void> Function(Uint8List) onWrite;
  final Duration debounce;

  Uint8List? _pending;
  Timer? _timer;
  bool _disposed = false;

  void schedule(Uint8List payload) {
    if (_disposed) return;
    _pending = payload;
    _timer?.cancel();
    _timer = Timer(debounce, () => unawaited(_drain()));
  }

  Future<void> flush() async {
    _timer?.cancel();
    await _drain();
  }

  Future<void> _drain() async {
    final payload = _pending;
    _pending = null;
    if (payload != null) {
      await onWrite(payload);
    }
  }

  void dispose() {
    _disposed = true;
    _timer?.cancel();
    _pending = null;
  }
}
```

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/core/ble/write_coalescer_test.dart
```
Expected: all 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add lib/core/ble/write_coalescer.dart test/core/ble/write_coalescer_test.dart
git commit -m "feat(ble): WriteCoalescer — trailing debounce for slider drags"
```

---

## Task 2: Persist the lamp password in inventory

The Control notifier needs the lamp's BLE-control password to write `CHAR_AUTH`. The wizard already collects it; we just need to store it on the `InventoryLamp` and pass it through `submit()`.

**Files:**
- Modify: `software/lamp-app-flutter/lib/features/inventory/domain/inventory_lamp.dart`
- Modify: `software/lamp-app-flutter/lib/features/onboarding/application/add_lamp_notifier.dart`
- Modify: `software/lamp-app-flutter/test/features/onboarding/add_lamp_notifier_test.dart`

- [ ] **Step 1: Extend the failing test**

Open `test/features/onboarding/add_lamp_notifier_test.dart`. In the `submit()` test, add after the inventory expectation:

```dart
    expect(inventory.first.controlPassword, 'secret');
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/onboarding/add_lamp_notifier_test.dart
```
Expected: compile error (`controlPassword` not a property of `InventoryLamp`).

- [ ] **Step 3: Add the field to InventoryLamp**

Edit `lib/features/inventory/domain/inventory_lamp.dart`:

```dart
@freezed
abstract class InventoryLamp with _$InventoryLamp {
  const factory InventoryLamp({
    required String id,
    required String name,
    String? controlPassword,
    int? lastSeenEpochMs,
    List<int>? lastShadeColor,
    List<int>? lastBaseColor,
  }) = _InventoryLamp;

  factory InventoryLamp.fromJson(Map<String, dynamic> json) =>
      _$InventoryLampFromJson(json);
}
```

Run codegen:
```bash
dart run build_runner build
```

- [ ] **Step 4: Pass password through submit()**

Edit `lib/features/onboarding/application/add_lamp_notifier.dart`, inside `submit()`:

```dart
      await ref.read(inventoryNotifierProvider.notifier).add(
            InventoryLamp(
              id: state.deviceId,
              name: state.name,
              controlPassword: state.password,
            ),
          );
```

Leave the `add(deviceId, name)` method (the one-tap add path) unchanged — passwords for *other people's* lamps are out of scope here; the Control notifier falls back to no-auth when password is null.

- [ ] **Step 5: Run, expect pass**

```bash
flutter test test/features/onboarding/add_lamp_notifier_test.dart
```

- [ ] **Step 6: Run the full suite to catch fallout**

```bash
flutter analyze && flutter test
```
Expected: all green.

- [ ] **Step 7: Commit**

```bash
git add lib/ test/
git commit -m "feat(inventory): store controlPassword on adopted lamps"
```

---

## Task 3: LampColor (RGBW + hex codec)

The firmware uses 4-channel RGBW for the LED strips and serializes colors as 8-char hex strings (`#RRGGBBWW`). The default shade `#000000FF` is "white channel only, RGB off" — losing the W byte would silently break shades that are pure-white. We carry W through the model even though Phase 2's color picker only exposes RGB.

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/domain/lamp_color.dart`
- Create: `software/lamp-app-flutter/test/features/control/domain/lamp_color_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/features/control/domain/lamp_color_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';

void main() {
  test('parses #RRGGBBWW into r/g/b/w', () {
    final c = LampColor.fromHex('#30FF80AA');
    expect(c.r, 0x30);
    expect(c.g, 0xFF);
    expect(c.b, 0x80);
    expect(c.w, 0xAA);
  });

  test('accepts hex without leading hash', () {
    expect(LampColor.fromHex('300783FF').w, 0xFF);
  });

  test('toHex round-trips exactly, upper-case, with #', () {
    final c = const LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF);
    expect(c.toHex(), '#300783FF');
  });

  test('throws FormatException on invalid hex length', () {
    expect(() => LampColor.fromHex('#FFF'), throwsFormatException);
  });

  test('withRgb keeps the W byte', () {
    final c = const LampColor(r: 0x10, g: 0x20, b: 0x30, w: 0xAA);
    final next = c.withRgb(r: 0xFF, g: 0xFF, b: 0xFF);
    expect(next.w, 0xAA);
    expect(next.r, 0xFF);
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/control/domain/lamp_color_test.dart
```

- [ ] **Step 3: Implement LampColor**

Create `lib/features/control/domain/lamp_color.dart`:

```dart
import 'package:flutter/painting.dart' show Color;

/// A lamp color carries an explicit white channel alongside RGB. The firmware
/// serializes these as `#RRGGBBWW` hex strings on every section payload.
class LampColor {
  const LampColor({
    required this.r,
    required this.g,
    required this.b,
    required this.w,
  });

  final int r;
  final int g;
  final int b;
  final int w;

  factory LampColor.fromHex(String input) {
    var s = input.startsWith('#') ? input.substring(1) : input;
    if (s.length != 8) {
      throw FormatException('LampColor.fromHex expects 8 hex chars, got "$input"');
    }
    return LampColor(
      r: int.parse(s.substring(0, 2), radix: 16),
      g: int.parse(s.substring(2, 4), radix: 16),
      b: int.parse(s.substring(4, 6), radix: 16),
      w: int.parse(s.substring(6, 8), radix: 16),
    );
  }

  String toHex() {
    String h(int v) => v.toRadixString(16).padLeft(2, '0').toUpperCase();
    return '#${h(r)}${h(g)}${h(b)}${h(w)}';
  }

  /// Convert RGB portion to a Flutter [Color] for swatch/preview rendering.
  /// The W byte is dropped — it only ever drives the physical strip's white
  /// channel, never the on-screen swatch.
  Color toSwatch() => Color.fromARGB(0xFF, r, g, b);

  LampColor withRgb({required int r, required int g, required int b}) =>
      LampColor(r: r, g: g, b: b, w: w);

  @override
  bool operator ==(Object other) =>
      other is LampColor &&
      other.r == r &&
      other.g == g &&
      other.b == b &&
      other.w == w;

  @override
  int get hashCode => Object.hash(r, g, b, w);

  @override
  String toString() => 'LampColor(${toHex()})';
}
```

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/features/control/domain/lamp_color_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/domain/lamp_color.dart test/features/control/domain/lamp_color_test.dart
git commit -m "feat(control): LampColor (RGBW + #RRGGBBWW codec)"
```

---

## Task 4: Section JSON models

`CHAR_LAMP_SECTION`, `CHAR_BASE_SECTION`, `CHAR_SHADE_SECTION` each return a JSON blob the firmware produces from `Config::asLampJson` / `asBaseJson` / `asShadeJson` (firmware: `software/lamp-os/src/config/config.cpp:250-296`). We model each shape minimally — only the fields Phase 2 reads.

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/domain/sections.dart`
- Create: `software/lamp-app-flutter/test/features/control/domain/sections_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/features/control/domain/sections_test.dart`:

```dart
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/domain/sections.dart';

void main() {
  test('LampSection parses brightness + name', () {
    final s = LampSection.fromJson(jsonDecode(
      '{"name":"jacko","brightness":42,"advancedEnabled":false}',
    ) as Map<String, dynamic>);
    expect(s.name, 'jacko');
    expect(s.brightness, 42);
  });

  test('BaseSection parses colors, ac, px', () {
    final s = BaseSection.fromJson(jsonDecode(
      '{"px":35,"ac":1,"bpp":4,"colors":["#300783FF","#FF0000AA"],"knockout":[]}',
    ) as Map<String, dynamic>);
    expect(s.px, 35);
    expect(s.ac, 1);
    expect(s.colors.length, 2);
    expect(s.colors[0], const LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF));
    expect(s.colors[1].w, 0xAA);
  });

  test('ShadeSection parses single color', () {
    final s = ShadeSection.fromJson(jsonDecode(
      '{"px":38,"bpp":4,"colors":["#000000FF"]}',
    ) as Map<String, dynamic>);
    expect(s.colors.single.w, 0xFF);
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/control/domain/sections_test.dart
```

- [ ] **Step 3: Implement sections**

Create `lib/features/control/domain/sections.dart`:

```dart
import 'lamp_color.dart';

/// CHAR_LAMP_SECTION payload, see firmware Config::asLampJson.
class LampSection {
  const LampSection({
    required this.name,
    required this.brightness,
    required this.advancedEnabled,
  });

  final String name;
  final int brightness;
  final bool advancedEnabled;

  factory LampSection.fromJson(Map<String, dynamic> json) => LampSection(
        name: (json['name'] as String?) ?? '',
        brightness: (json['brightness'] as num?)?.toInt() ?? 100,
        advancedEnabled: json['advancedEnabled'] as bool? ?? false,
      );
}

/// CHAR_BASE_SECTION payload, see firmware Config::asBaseJson.
class BaseSection {
  const BaseSection({
    required this.px,
    required this.ac,
    required this.bpp,
    required this.colors,
  });

  final int px;
  final int ac;
  final int bpp;
  final List<LampColor> colors;

  factory BaseSection.fromJson(Map<String, dynamic> json) => BaseSection(
        px: (json['px'] as num?)?.toInt() ?? 35,
        ac: (json['ac'] as num?)?.toInt() ?? 0,
        bpp: (json['bpp'] as num?)?.toInt() ?? 4,
        colors: ((json['colors'] as List?) ?? const [])
            .map((e) => LampColor.fromHex(e as String))
            .toList(),
      );
}

/// CHAR_SHADE_SECTION payload, see firmware Config::asShadeJson.
class ShadeSection {
  const ShadeSection({
    required this.px,
    required this.bpp,
    required this.colors,
  });

  final int px;
  final int bpp;
  final List<LampColor> colors;

  factory ShadeSection.fromJson(Map<String, dynamic> json) => ShadeSection(
        px: (json['px'] as num?)?.toInt() ?? 38,
        bpp: (json['bpp'] as num?)?.toInt() ?? 4,
        colors: ((json['colors'] as List?) ?? const [])
            .map((e) => LampColor.fromHex(e as String))
            .toList(),
      );
}
```

These are plain Dart classes, not Freezed — they're read-only payload wrappers we never mutate, and copyWith / equality aren't needed.

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/features/control/domain/sections_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/domain/sections.dart test/features/control/domain/sections_test.dart
git commit -m "feat(control): LampSection / BaseSection / ShadeSection JSON parsers"
```

---

## Task 5: AuthClient

Writes `CHAR_AUTH` with the lamp's password. The firmware (`ble_control.cpp:125-137`) accepts the bytes and marks the connection authed if they match `lamp.password`; otherwise writes silently no-op. We treat the auth write as fire-and-forget — there's no positive acknowledgement; the next characteristic write will succeed or fail based on `isAuthed()`.

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/application/auth_client.dart`
- Create: `software/lamp-app-flutter/test/features/control/application/auth_client_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/features/control/application/auth_client_test.dart`:

```dart
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/auth_client.dart';

void main() {
  test('writes CHAR_AUTH with the utf8-encoded password', () async {
    final ble = InMemoryBleClient();
    await ble.connect('dev1');
    final auth = AuthClient(ble: ble);

    await auth.authenticate(deviceId: 'dev1', password: 'open sesame');

    final written = await ble.read(
        'dev1', BleUuids.controlService, BleUuids.auth);
    expect(utf8.decode(written), 'open sesame');
  });

  test('no-op when password is null or empty', () async {
    final ble = InMemoryBleClient();
    await ble.connect('dev1');
    final auth = AuthClient(ble: ble);

    await auth.authenticate(deviceId: 'dev1', password: null);
    await auth.authenticate(deviceId: 'dev1', password: '');

    // No write went through.
    expect(
      () => ble.read('dev1', BleUuids.controlService, BleUuids.auth),
      throwsA(isA<BleNotFound>()),
    );
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/control/application/auth_client_test.dart
```

- [ ] **Step 3: Implement AuthClient**

Create `lib/features/control/application/auth_client.dart`:

```dart
import 'dart:convert';
import 'dart:typed_data';

import '../../../core/ble/ble_client.dart';
import '../../../core/ble/uuids.dart';

/// Writes [BleUuids.auth] with the lamp's password so subsequent control
/// writes are accepted by the firmware's per-connection auth gate. The
/// firmware allows open access when no password is set (lamp.password is
/// empty), so this is a no-op for [password] == null or "".
class AuthClient {
  AuthClient({required this.ble});

  final BleClient ble;

  Future<void> authenticate({
    required String deviceId,
    required String? password,
  }) async {
    if (password == null || password.isEmpty) return;
    await ble.write(
      deviceId,
      BleUuids.controlService,
      BleUuids.auth,
      Uint8List.fromList(utf8.encode(password)),
    );
  }
}
```

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/features/control/application/auth_client_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/application/auth_client.dart test/features/control/application/auth_client_test.dart
git commit -m "feat(control): AuthClient — writes CHAR_AUTH with lamp password"
```

---

## Task 6: ControlState

A Freezed model carrying the parsed sections plus any error / loading flag. Reads only; mutations happen via the notifier (which then refreshes state).

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/application/control_state.dart`

- [ ] **Step 1: Implement (no test — pure data class, exercised by notifier tests)**

Create `lib/features/control/application/control_state.dart`:

```dart
import 'package:freezed_annotation/freezed_annotation.dart';

import '../domain/sections.dart';

part 'control_state.freezed.dart';

/// Combined state for the Control screen. Populated by ControlNotifier after
/// connect+auth+per-section reads. Not JSON-serialized — purely in-memory.
@freezed
abstract class ControlState with _$ControlState {
  const factory ControlState({
    required LampSection lamp,
    required BaseSection base,
    required ShadeSection shade,
  }) = _ControlState;
}
```

(No `fromJson` — this is reconstructed from three separate section reads.)

- [ ] **Step 2: Run codegen**

```bash
dart run build_runner build
```

- [ ] **Step 3: Verify analyzer is clean**

```bash
flutter analyze lib/features/control/application/control_state.dart
```

- [ ] **Step 4: Commit**

```bash
git add lib/features/control/application/control_state.dart lib/features/control/application/control_state.freezed.dart
git commit -m "feat(control): ControlState — combined section payload"
```

---

## Task 7: ControlNotifier

The async family that drives the screen. Responsibilities:
1. On build: read inventory for the password, connect over BLE, write `CHAR_AUTH`, read all three sections, parse into `ControlState`.
2. Expose mutation methods: `setBrightness(int)`, `setShadeColor(LampColor)`, `setBaseColors(List<LampColor>)`, `setBaseAc(int)`. Each one debounces through a `WriteCoalescer` keyed on the characteristic and optimistically updates local state.
3. On dispose: flush + dispose all coalescers; disconnect.

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/application/control_notifier.dart`
- Create: `software/lamp-app-flutter/test/features/control/application/control_notifier_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/features/control/application/control_notifier_test.dart`:

```dart
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:shared_preferences/shared_preferences.dart';

const _devId = 'dev1';

Future<void> _seed(InMemoryBleClient ble) async {
  // Pretend the lamp's sections are already published. Seed by writing
  // through the client while "connected" — the test acts as the firmware
  // here. The notifier will then read these back.
  await ble.connect(_devId);
  await ble.write(_devId, BleUuids.controlService, BleUuids.lampSection,
      Uint8List.fromList(utf8.encode(
        '{"name":"jacko","brightness":42,"advancedEnabled":false}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.baseSection,
      Uint8List.fromList(utf8.encode(
        '{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"],"knockout":[]}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.shadeSection,
      Uint8List.fromList(utf8.encode(
        '{"px":38,"bpp":4,"colors":["#000000FF"]}',
      )));
  await ble.disconnect(_devId);
}

void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  test('on build: connects, auths, populates state from sections', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));

    final state =
        await c.read(controlNotifierProvider(_devId).future);
    expect(state.lamp.brightness, 42);
    expect(state.base.colors.single.toHex(), '#300783FF');
    expect(state.shade.colors.single.toHex(), '#000000FF');

    final auth = await ble.read(_devId, BleUuids.controlService, BleUuids.auth);
    expect(utf8.decode(auth), 'secret');
  });

  test('setBrightness optimistically updates and writes after debounce',
      () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    await c
        .read(controlNotifierProvider(_devId).notifier)
        .setBrightness(80);
    // Optimistic update is visible immediately.
    expect(
      c.read(controlNotifierProvider(_devId)).value!.lamp.brightness,
      80,
    );
    // Drain the debounce window.
    await Future<void>.delayed(const Duration(milliseconds: 60));
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.brightness);
    expect(written.single, 80);
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/control/application/control_notifier_test.dart
```

- [ ] **Step 3: Implement ControlNotifier**

Create `lib/features/control/application/control_notifier.dart`:

```dart
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client.dart';
import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/uuids.dart';
import '../../../core/ble/write_coalescer.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../domain/lamp_color.dart';
import '../domain/sections.dart';
import 'auth_client.dart';
import 'control_state.dart';

part 'control_notifier.g.dart';

const _writeDebounce = Duration(milliseconds: 30);

@Riverpod(keepAlive: false)
class ControlNotifier extends _$ControlNotifier {
  late final WriteCoalescer _brightnessWriter;
  late final WriteCoalescer _shadeColorsWriter;
  late final WriteCoalescer _baseColorsWriter;

  @override
  Future<ControlState> build(String deviceId) async {
    final ble = ref.read(bleClientProvider);
    final inv = await ref.read(inventoryNotifierProvider.future);
    final lamp = inv.firstWhere(
      (l) => l.id == deviceId,
      orElse: () => throw StateError('lamp $deviceId not in inventory'),
    );

    await ble.connect(deviceId);
    await AuthClient(ble: ble)
        .authenticate(deviceId: deviceId, password: lamp.controlPassword);

    Future<Map<String, dynamic>> readJson(String charUuid) async {
      final bytes = await ble.read(deviceId, BleUuids.controlService, charUuid);
      return jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
    }

    final lampJson = await readJson(BleUuids.lampSection);
    final baseJson = await readJson(BleUuids.baseSection);
    final shadeJson = await readJson(BleUuids.shadeSection);

    _brightnessWriter = WriteCoalescer(
      onWrite: (v) =>
          ble.write(deviceId, BleUuids.controlService, BleUuids.brightness, v),
      debounce: _writeDebounce,
    );
    _shadeColorsWriter = WriteCoalescer(
      onWrite: (v) =>
          ble.write(deviceId, BleUuids.controlService, BleUuids.shadeColors, v),
      debounce: _writeDebounce,
    );
    _baseColorsWriter = WriteCoalescer(
      onWrite: (v) =>
          ble.write(deviceId, BleUuids.controlService, BleUuids.baseColors, v),
      debounce: _writeDebounce,
    );

    ref.onDispose(() {
      _brightnessWriter.dispose();
      _shadeColorsWriter.dispose();
      _baseColorsWriter.dispose();
      ble.disconnect(deviceId);
    });

    return ControlState(
      lamp: LampSection.fromJson(lampJson),
      base: BaseSection.fromJson(baseJson),
      shade: ShadeSection.fromJson(shadeJson),
    );
  }

  Future<void> setBrightness(int value) async {
    final v = value.clamp(0, 100);
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      lamp: LampSection(
        name: cur.lamp.name,
        brightness: v,
        advancedEnabled: cur.lamp.advancedEnabled,
      ),
    ));
    _brightnessWriter.schedule(Uint8List.fromList([v]));
  }

  Future<void> setShadeColor(LampColor color) async {
    final cur = state.value;
    if (cur == null) return;
    final colors = [color];
    state = AsyncData(cur.copyWith(
      shade: ShadeSection(px: cur.shade.px, bpp: cur.shade.bpp, colors: colors),
    ));
    _shadeColorsWriter.schedule(_encodeColors(colors));
  }

  Future<void> setBaseColors(List<LampColor> colors) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: cur.base.px,
        ac: cur.base.ac.clamp(0, colors.isEmpty ? 0 : colors.length - 1),
        bpp: cur.base.bpp,
        colors: colors,
      ),
    ));
    _baseColorsWriter.schedule(_encodeColors(colors));
  }

  Future<void> setBaseAc(int index) async {
    final cur = state.value;
    if (cur == null) return;
    final clamped =
        index.clamp(0, cur.base.colors.isEmpty ? 0 : cur.base.colors.length - 1);
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: cur.base.px,
        ac: clamped,
        bpp: cur.base.bpp,
        colors: cur.base.colors,
      ),
    ));
    // Active-color index is part of the base settings blob, not its own char;
    // for v1 we update it alongside the colors write so the firmware picks it
    // up on the next CHAR_BASE_COLORS payload via the full base settings save
    // (which Phase 5's Setup screen will own). For the visible Control-tab
    // session it's enough to reflect locally; a full save flushes the index
    // on submit. No write fired here.
  }

  Uint8List _encodeColors(List<LampColor> colors) {
    final arr = colors.map((c) => c.toHex()).toList();
    return Uint8List.fromList(utf8.encode(jsonEncode(arr)));
  }
}
```

- [ ] **Step 4: Run codegen + test**

```bash
dart run build_runner build
flutter test test/features/control/application/control_notifier_test.dart
```
Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/application/control_notifier.dart lib/features/control/application/control_notifier.g.dart test/features/control/application/control_notifier_test.dart
git commit -m "feat(control): ControlNotifier — connect/auth/read + debounced writes"
```

---

## Task 8: ColorPickerSheet (shared)

A modal bottom sheet that opens with a starting color, lets the user pick a new one (HSV via `flutter_colorpicker`), and returns the picked `LampColor` (preserving the original W byte) when Save is tapped, or `null` if dismissed.

**Files:**
- Modify: `software/lamp-app-flutter/pubspec.yaml`
- Create: `software/lamp-app-flutter/lib/features/control/presentation/widgets/color_picker_sheet.dart`

- [ ] **Step 1: Add dependency**

Add to `pubspec.yaml` under `dependencies:`:

```yaml
  flutter_colorpicker: ^1.1.0
```

Then `flutter pub get`.

- [ ] **Step 2: Implement the sheet**

Create `lib/features/control/presentation/widgets/color_picker_sheet.dart`:

```dart
import 'package:flutter/material.dart';
import 'package:flutter_colorpicker/flutter_colorpicker.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';

/// Opens a modal bottom sheet to pick a color, returning the chosen
/// [LampColor] or null if cancelled. The W byte of [initial] is carried
/// through untouched — the picker only edits R/G/B.
Future<LampColor?> showColorPickerSheet(
  BuildContext context, {
  required LampColor initial,
  String title = 'Pick a color',
}) {
  return showModalBottomSheet<LampColor>(
    context: context,
    isScrollControlled: true,
    backgroundColor: BrandColors.midnightBlack,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) => _ColorPickerSheet(initial: initial, title: title),
  );
}

class _ColorPickerSheet extends StatefulWidget {
  const _ColorPickerSheet({required this.initial, required this.title});
  final LampColor initial;
  final String title;

  @override
  State<_ColorPickerSheet> createState() => _ColorPickerSheetState();
}

class _ColorPickerSheetState extends State<_ColorPickerSheet> {
  late Color _picked = widget.initial.toSwatch();

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: EdgeInsets.only(
        bottom: MediaQuery.of(context).viewInsets.bottom,
      ),
      child: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Text(
                widget.title,
                style: const TextStyle(
                  color: BrandColors.lampWhite,
                  fontSize: 16,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 12),
              ColorPicker(
                pickerColor: _picked,
                onColorChanged: (c) => setState(() => _picked = c),
                pickerAreaHeightPercent: 0.6,
                enableAlpha: false,
                labelTypes: const [],
                hexInputBar: true,
              ),
              const SizedBox(height: 12),
              Row(
                children: [
                  TextButton(
                    onPressed: () => Navigator.pop(context),
                    child: const Text('Cancel'),
                  ),
                  const Spacer(),
                  FilledButton(
                    onPressed: () => Navigator.pop(
                      context,
                      widget.initial.withRgb(
                        r: _picked.red,
                        g: _picked.green,
                        b: _picked.blue,
                      ),
                    ),
                    child: const Text('Save'),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}
```

- [ ] **Step 3: Analyzer check**

```bash
flutter analyze lib/features/control/presentation/widgets/color_picker_sheet.dart
```

- [ ] **Step 4: Commit**

```bash
git add pubspec.yaml pubspec.lock lib/features/control/presentation/widgets/color_picker_sheet.dart
git commit -m "feat(control): shared ColorPickerSheet (HSV + hex via flutter_colorpicker)"
```

---

## Task 9: BrightnessCard

Brand-styled card with a slider 0-100 and a trailing percentage label. Calls `notifier.setBrightness(int)` on every drag tick.

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/presentation/widgets/brightness_card.dart`
- Create: `software/lamp-app-flutter/test/features/control/presentation/widgets/brightness_card_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/features/control/presentation/widgets/brightness_card_test.dart`:

```dart
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/presentation/widgets/brightness_card.dart';

void main() {
  testWidgets('renders the percentage label and slider', (tester) async {
    int? changed;
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BrightnessCard(value: 42, onChanged: (v) => changed = v),
      ),
    ));
    expect(find.text('42%'), findsOneWidget);
    expect(find.byType(Slider), findsOneWidget);

    // Drag the slider — gesture exactness isn't important, just that the
    // callback fires with an int in range.
    final slider = tester.widget<Slider>(find.byType(Slider));
    slider.onChanged!(60);
    expect(changed, 60);
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/control/presentation/widgets/brightness_card_test.dart
```

- [ ] **Step 3: Implement BrightnessCard**

Create `lib/features/control/presentation/widgets/brightness_card.dart`:

```dart
import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';

class BrightnessCard extends StatelessWidget {
  const BrightnessCard({
    super.key,
    required this.value,
    required this.onChanged,
  });

  final int value;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.04),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Text(
                'Brightness',
                style: TextStyle(
                  color: BrandColors.lampWhite,
                  fontSize: 14,
                  fontWeight: FontWeight.w600,
                  letterSpacing: 0.4,
                ),
              ),
              const Spacer(),
              Text(
                '$value%',
                style: const TextStyle(
                  color: BrandColors.fogGrey,
                  fontSize: 12,
                ),
              ),
            ],
          ),
          Slider(
            min: 0,
            max: 100,
            divisions: 100,
            value: value.toDouble(),
            onChanged: (v) => onChanged(v.toInt()),
          ),
        ],
      ),
    );
  }
}
```

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/features/control/presentation/widgets/brightness_card_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/presentation/widgets/brightness_card.dart test/features/control/presentation/widgets/brightness_card_test.dart
git commit -m "feat(control): BrightnessCard"
```

---

## Task 10: ShadeCard

Single circular swatch + hex label. Tap → opens the shared color picker; result is passed to `onChanged`.

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/presentation/widgets/shade_card.dart`
- Create: `software/lamp-app-flutter/test/features/control/presentation/widgets/shade_card_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/features/control/presentation/widgets/shade_card_test.dart`:

```dart
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/shade_card.dart';

void main() {
  testWidgets('renders title, hex label, and swatch', (tester) async {
    const color = LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF);
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: ShadeCard(color: color, onChanged: (_) {}),
      ),
    ));
    expect(find.text('Shade'), findsOneWidget);
    expect(find.text('#300783FF'), findsOneWidget);
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/control/presentation/widgets/shade_card_test.dart
```

- [ ] **Step 3: Implement ShadeCard**

Create `lib/features/control/presentation/widgets/shade_card.dart`:

```dart
import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';
import 'color_picker_sheet.dart';

class ShadeCard extends StatelessWidget {
  const ShadeCard({super.key, required this.color, required this.onChanged});

  final LampColor color;
  final ValueChanged<LampColor> onChanged;

  Future<void> _onTap(BuildContext context) async {
    final picked = await showColorPickerSheet(
      context,
      initial: color,
      title: 'Pick a shade color',
    );
    if (picked != null) onChanged(picked);
  }

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: () => _onTap(context),
      borderRadius: BorderRadius.circular(12),
      child: Container(
        margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
        padding: const EdgeInsets.all(16),
        decoration: BoxDecoration(
          color: Colors.white.withValues(alpha: 0.04),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
        ),
        child: Row(
          children: [
            Container(
              width: 36,
              height: 36,
              decoration: BoxDecoration(
                color: color.toSwatch(),
                shape: BoxShape.circle,
                border: Border.all(color: Colors.white.withValues(alpha: 0.12)),
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Shade',
                    style: TextStyle(
                      color: BrandColors.lampWhite,
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                      letterSpacing: 0.4,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    color.toHex(),
                    style: const TextStyle(
                      color: BrandColors.fogGrey,
                      fontSize: 12,
                      fontFamily: 'monospace',
                    ),
                  ),
                ],
              ),
            ),
            const Icon(Icons.chevron_right, color: BrandColors.slateGrey),
          ],
        ),
      ),
    );
  }
}
```

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/features/control/presentation/widgets/shade_card_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/presentation/widgets/shade_card.dart test/features/control/presentation/widgets/shade_card_test.dart
git commit -m "feat(control): ShadeCard"
```

---

## Task 11: BaseCard (compact)

Compact base preview: a vertical 60-pixel-tall ribbon showing the gradient blend on the left, a mini-stack of swatches on the right, with subtitle "Base · N stops". Tap opens the editor (Task 12).

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/presentation/widgets/base_card.dart`
- Create: `software/lamp-app-flutter/test/features/control/presentation/widgets/base_card_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/features/control/presentation/widgets/base_card_test.dart`:

```dart
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/base_card.dart';

void main() {
  testWidgets('shows stop count subtitle and a ribbon for N stops',
      (tester) async {
    final colors = const [
      LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0),
      LampColor(r: 0xFF, g: 0x00, b: 0x00, w: 0),
    ];
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BaseCard(colors: colors, activeIndex: 0, onTap: () {}),
      ),
    ));
    expect(find.text('Base · 2 stops'), findsOneWidget);
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/control/presentation/widgets/base_card_test.dart
```

- [ ] **Step 3: Implement BaseCard**

Create `lib/features/control/presentation/widgets/base_card.dart`:

```dart
import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';

class BaseCard extends StatelessWidget {
  const BaseCard({
    super.key,
    required this.colors,
    required this.activeIndex,
    required this.onTap,
  });

  final List<LampColor> colors;
  final int activeIndex;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(12),
      child: Container(
        margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
        padding: const EdgeInsets.all(16),
        decoration: BoxDecoration(
          color: Colors.white.withValues(alpha: 0.04),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
        ),
        child: Row(
          children: [
            // Gradient ribbon.
            Container(
              width: 28,
              height: 60,
              decoration: BoxDecoration(
                borderRadius: BorderRadius.circular(6),
                gradient: LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: colors.isEmpty
                      ? const [Colors.black, Colors.black]
                      : colors.map((c) => c.toSwatch()).toList(),
                ),
              ),
            ),
            const SizedBox(width: 12),
            // Mini stack of stop swatches.
            Column(
              children: [
                for (var i = 0; i < colors.length; i++)
                  Container(
                    margin: const EdgeInsets.only(bottom: 4),
                    width: 16,
                    height: 16,
                    decoration: BoxDecoration(
                      color: colors[i].toSwatch(),
                      shape: BoxShape.circle,
                      border: Border.all(
                        color: i == activeIndex
                            ? BrandColors.glowPink
                            : Colors.white.withValues(alpha: 0.12),
                        width: i == activeIndex ? 2 : 1,
                      ),
                    ),
                  ),
              ],
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Base',
                    style: TextStyle(
                      color: BrandColors.lampWhite,
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                      letterSpacing: 0.4,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    'Base · ${colors.length} stops',
                    style: const TextStyle(
                      color: BrandColors.fogGrey,
                      fontSize: 12,
                    ),
                  ),
                ],
              ),
            ),
            const Icon(Icons.chevron_right, color: BrandColors.slateGrey),
          ],
        ),
      ),
    );
  }
}
```

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/features/control/presentation/widgets/base_card_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/presentation/widgets/base_card.dart test/features/control/presentation/widgets/base_card_test.dart
git commit -m "feat(control): BaseCard — compact gradient preview + active dot"
```

---

## Task 12: BaseEditorSheet (full editor)

Full-height (90%) modal bottom sheet. Vertical list of stops (drag-to-reorder, swatch, hex, ✕ remove). "+ Add stop" row when < 5 stops. Active-color picker: tap a stop to make it the active color (`ac`). Tap the swatch on a stop opens the shared color picker. All changes flow back to the parent via the same `onChanged(List<LampColor>)` and `onActiveChanged(int)` callbacks; live writes happen in the parent (ControlNotifier already debounces).

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/presentation/widgets/base_editor_sheet.dart`
- Create: `software/lamp-app-flutter/test/features/control/presentation/widgets/base_editor_sheet_test.dart`

- [ ] **Step 1: Write the failing test**

Create `test/features/control/presentation/widgets/base_editor_sheet_test.dart`:

```dart
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/base_editor_sheet.dart';

void main() {
  testWidgets('renders one row per stop and the add-stop CTA when < 5',
      (tester) async {
    final colors = const [
      LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0),
      LampColor(r: 0xFF, g: 0x00, b: 0x00, w: 0),
    ];
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BaseEditorSheet(
          colors: colors,
          activeIndex: 0,
          onColorsChanged: (_) {},
          onActiveChanged: (_) {},
        ),
      ),
    ));
    expect(find.text('#300783'), findsOneWidget);
    expect(find.text('#FF0000'), findsOneWidget);
    expect(find.text('+ Add stop'), findsOneWidget);
  });

  testWidgets('hides the add-stop CTA at 5 stops', (tester) async {
    final colors = List.generate(
      5,
      (i) => LampColor(r: i * 20, g: 0, b: 0, w: 0),
    );
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BaseEditorSheet(
          colors: colors,
          activeIndex: 0,
          onColorsChanged: (_) {},
          onActiveChanged: (_) {},
        ),
      ),
    ));
    expect(find.text('+ Add stop'), findsNothing);
  });
}
```

- [ ] **Step 2: Run, expect failure**

```bash
flutter test test/features/control/presentation/widgets/base_editor_sheet_test.dart
```

- [ ] **Step 3: Implement BaseEditorSheet**

Create `lib/features/control/presentation/widgets/base_editor_sheet.dart`:

```dart
import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';
import 'color_picker_sheet.dart';

class BaseEditorSheet extends StatelessWidget {
  const BaseEditorSheet({
    super.key,
    required this.colors,
    required this.activeIndex,
    required this.onColorsChanged,
    required this.onActiveChanged,
  });

  final List<LampColor> colors;
  final int activeIndex;
  final ValueChanged<List<LampColor>> onColorsChanged;
  final ValueChanged<int> onActiveChanged;

  Future<void> _editStop(BuildContext context, int i) async {
    final picked = await showColorPickerSheet(
      context,
      initial: colors[i],
      title: 'Stop ${i + 1}',
    );
    if (picked != null) {
      final next = [...colors];
      next[i] = picked;
      onColorsChanged(next);
    }
  }

  void _removeStop(int i) {
    if (colors.length <= 1) return;
    final next = [...colors]..removeAt(i);
    onColorsChanged(next);
    if (activeIndex >= next.length) onActiveChanged(next.length - 1);
  }

  void _addStop() {
    if (colors.length >= 5) return;
    onColorsChanged([
      ...colors,
      const LampColor(r: 0xFF, g: 0xFF, b: 0xFF, w: 0),
    ]);
  }

  void _reorder(int oldIndex, int newIndex) {
    var ni = newIndex;
    if (ni > oldIndex) ni -= 1;
    final next = [...colors];
    final picked = next.removeAt(oldIndex);
    next.insert(ni, picked);
    onColorsChanged(next);
    if (activeIndex == oldIndex) {
      onActiveChanged(ni);
    }
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            const Row(
              children: [
                Text(
                  'Base gradient',
                  style: TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 16,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                Spacer(),
              ],
            ),
            const SizedBox(height: 12),
            Expanded(
              child: ReorderableListView.builder(
                itemCount: colors.length,
                onReorder: _reorder,
                buildDefaultDragHandles: false,
                itemBuilder: (ctx, i) {
                  final stop = colors[i];
                  return ListTile(
                    key: ValueKey('stop-$i'),
                    onTap: () => onActiveChanged(i),
                    leading: ReorderableDragStartListener(
                      index: i,
                      child: const Icon(Icons.drag_indicator,
                          color: BrandColors.slateGrey),
                    ),
                    title: Row(
                      children: [
                        GestureDetector(
                          onTap: () => _editStop(context, i),
                          child: Container(
                            width: 28,
                            height: 28,
                            decoration: BoxDecoration(
                              color: stop.toSwatch(),
                              shape: BoxShape.circle,
                              border: Border.all(
                                color: i == activeIndex
                                    ? BrandColors.glowPink
                                    : Colors.white.withValues(alpha: 0.12),
                                width: i == activeIndex ? 2 : 1,
                              ),
                            ),
                          ),
                        ),
                        const SizedBox(width: 12),
                        Text(
                          '#${stop.toHex().substring(1, 7)}',
                          style: const TextStyle(
                            color: BrandColors.fogGrey,
                            fontSize: 12,
                            fontFamily: 'monospace',
                          ),
                        ),
                      ],
                    ),
                    trailing: IconButton(
                      icon: const Icon(Icons.close,
                          color: BrandColors.slateGrey),
                      onPressed: colors.length <= 1
                          ? null
                          : () => _removeStop(i),
                    ),
                  );
                },
              ),
            ),
            if (colors.length < 5)
              TextButton(
                onPressed: _addStop,
                child: const Text('+ Add stop'),
              ),
          ],
        ),
      ),
    );
  }
}

/// Convenience opener so consumers don't have to thread Riverpod through
/// showModalBottomSheet; just hand it the current state + callbacks.
Future<void> showBaseEditorSheet(
  BuildContext context, {
  required List<LampColor> colors,
  required int activeIndex,
  required ValueChanged<List<LampColor>> onColorsChanged,
  required ValueChanged<int> onActiveChanged,
}) {
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    backgroundColor: BrandColors.midnightBlack,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) => FractionallySizedBox(
      heightFactor: 0.9,
      child: BaseEditorSheet(
        colors: colors,
        activeIndex: activeIndex,
        onColorsChanged: onColorsChanged,
        onActiveChanged: onActiveChanged,
      ),
    ),
  );
}
```

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/features/control/presentation/widgets/base_editor_sheet_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add lib/features/control/presentation/widgets/base_editor_sheet.dart test/features/control/presentation/widgets/base_editor_sheet_test.dart
git commit -m "feat(control): BaseEditorSheet — reorderable stops + per-stop picker"
```

---

## Task 13: ConnectingView (critter + "Connecting…")

Replaces the placeholder spinner in the loading state. Picks one of the lamplit-web critter SVGs and gives the connect-and-read pause a piece of character. Bundle SVGs into the Flutter project's assets and render with `flutter_svg`.

**Files:**
- Copy (file moves): `/Users/jerrett/projects/lamplit-web/public/critter-1.svg .. critter-7.svg` → `software/lamp-app-flutter/assets/critters/`
- Modify: `software/lamp-app-flutter/pubspec.yaml` (add `flutter_svg` + asset path)
- Create: `software/lamp-app-flutter/lib/features/control/presentation/widgets/connecting_view.dart`

- [ ] **Step 1: Add flutter_svg dependency**

In `pubspec.yaml` under `dependencies:`:
```yaml
  flutter_svg: ^2.0.10
```

Then under `flutter:`:
```yaml
  assets:
    - assets/critters/
```

- [ ] **Step 2: Copy the critter SVGs into the project**

```bash
mkdir -p software/lamp-app-flutter/assets/critters
cp /Users/jerrett/projects/lamplit-web/public/critter-1.svg \
   /Users/jerrett/projects/lamplit-web/public/critter-3.svg \
   /Users/jerrett/projects/lamplit-web/public/critter-5.svg \
   /Users/jerrett/projects/lamplit-web/public/critter-7.svg \
   software/lamp-app-flutter/assets/critters/
```

(Use the odd-numbered set — the asset-* variants are sketch fragments, not finished critters.)

- [ ] **Step 3: Resolve deps**

```bash
flutter pub get
```

- [ ] **Step 4: Implement ConnectingView**

Create `lib/features/control/presentation/widgets/connecting_view.dart`:

```dart
import 'dart:math';

import 'package:flutter/material.dart';
import 'package:flutter_svg/flutter_svg.dart';

import '../../../../core/theme/brand_colors.dart';

const _critters = [
  'assets/critters/critter-1.svg',
  'assets/critters/critter-3.svg',
  'assets/critters/critter-5.svg',
  'assets/critters/critter-7.svg',
];

/// Full-screen "we're talking to the lamp" state — a critter SVG that gently
/// pulses, with a single line of text below. Picks a critter deterministically
/// from [deviceId] so the same lamp always shows the same friend.
class ConnectingView extends StatefulWidget {
  const ConnectingView({super.key, required this.deviceId});
  final String deviceId;

  @override
  State<ConnectingView> createState() => _ConnectingViewState();
}

class _ConnectingViewState extends State<ConnectingView>
    with SingleTickerProviderStateMixin {
  late final AnimationController _ctrl = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 1400),
  )..repeat(reverse: true);

  late final Animation<double> _scale = Tween(begin: 0.96, end: 1.04)
      .chain(CurveTween(curve: Curves.easeInOut))
      .animate(_ctrl);

  String get _critter {
    final i = widget.deviceId.codeUnits.fold<int>(0, (a, b) => a + b);
    return _critters[i.abs() % _critters.length];
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          ScaleTransition(
            scale: _scale,
            child: SvgPicture.asset(
              _critter,
              width: 160,
              height: 160,
            ),
          ),
          const SizedBox(height: 24),
          const Text(
            'Connecting…',
            style: TextStyle(
              color: BrandColors.fogGrey,
              fontSize: 14,
              letterSpacing: 0.5,
            ),
          ),
        ],
      ),
    );
  }
}
```

(The deterministic pick by `deviceId` is a small detail — same lamp → same critter every session, so the lamp "has" a critter friend.)

- [ ] **Step 5: Commit**

```bash
git add pubspec.yaml pubspec.lock assets/ lib/features/control/presentation/widgets/connecting_view.dart
git commit -m "feat(control): ConnectingView — critter + 'Connecting…' loading state"
```

---

## Task 14: ControlScreen + wire-up

Replace the placeholder with the real screen. Reads `controlNotifierProvider(deviceId)` and renders loading / error / data states; in the data state, stacks the three cards and routes interactions to the notifier.

**Files:**
- Create: `software/lamp-app-flutter/lib/features/control/presentation/control_screen.dart`
- Modify: `software/lamp-app-flutter/lib/features/lamp_shell/presentation/lamp_shell.dart`
- Delete: `software/lamp-app-flutter/lib/features/lamp_shell/presentation/control_placeholder.dart`

- [ ] **Step 1: Implement ControlScreen**

Create `lib/features/control/presentation/control_screen.dart`:

```dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../application/control_notifier.dart';
import '../domain/lamp_color.dart';
import 'widgets/base_card.dart';
import 'widgets/base_editor_sheet.dart';
import 'widgets/brightness_card.dart';
import 'widgets/connecting_view.dart';
import 'widgets/shade_card.dart';

const _blackShade = LampColor(r: 0, g: 0, b: 0, w: 0);

class ControlScreen extends ConsumerWidget {
  const ControlScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final async = ref.watch(controlNotifierProvider(lampId));
    return async.when(
      loading: () => ConnectingView(deviceId: lampId),
      error: (e, _) => Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Text(
            'Could not reach this lamp: $e',
            style: const TextStyle(color: BrandColors.fogGrey),
            textAlign: TextAlign.center,
          ),
        ),
      ),
      data: (state) {
        final notifier = ref.read(controlNotifierProvider(lampId).notifier);
        return ListView(
          padding: const EdgeInsets.symmetric(vertical: 8),
          children: [
            BrightnessCard(
              value: state.lamp.brightness,
              onChanged: notifier.setBrightness,
            ),
            ShadeCard(
              color: state.shade.colors.isEmpty
                  ? _blackShade
                  : state.shade.colors.single,
              onChanged: notifier.setShadeColor,
            ),
            BaseCard(
              colors: state.base.colors,
              activeIndex: state.base.ac,
              onTap: () => showBaseEditorSheet(
                context,
                colors: state.base.colors,
                activeIndex: state.base.ac,
                onColorsChanged: notifier.setBaseColors,
                onActiveChanged: notifier.setBaseAc,
              ),
            ),
          ],
        );
      },
    );
  }
}

// Defensive: if a lamp reports zero shade colors, show black rather than
// crashing on `.single`. Live lamps always emit at least one. The
// `_blackShade` constant is declared at the top of this file alongside
// the imports.
```

- [ ] **Step 2: Swap the placeholder in LampShell**

Edit `lib/features/lamp_shell/presentation/lamp_shell.dart`:

```dart
import '../../../features/control/presentation/control_screen.dart';
```

Replace `import 'control_placeholder.dart';` with the above, then change the switch arm:

```dart
LampTab.control => ControlScreen(lampId: widget.lampId),
```

(Same arg name as before, just a different class.)

Then delete the now-unused file:
```bash
git rm lib/features/lamp_shell/presentation/control_placeholder.dart
```

- [ ] **Step 3: Update the LampShell smoke test if it imports the placeholder**

Open `test/features/lamp_shell/lamp_shell_test.dart`. If it asserts on `ControlPlaceholder`, replace those expectations with assertions on a known control-screen element (e.g. `expect(find.byType(CircularProgressIndicator), findsOneWidget)` since the notifier will throw in tests without a fully-set-up container — that's OK for the loading state).

- [ ] **Step 4: Run analyzer + tests**

```bash
flutter analyze && flutter test
```
Expected: green.

- [ ] **Step 5: Commit**

```bash
git add lib/ test/
git commit -m "feat(control): ControlScreen — real Control tab, replaces placeholder"
```

---

## Task 15: Manual phone test plan + sign-off

No automated coverage for the BLE side; verify on hardware.

**Files:**
- Modify: `software/lamp-app-flutter/test/manual/v1-checklist.md` (create if missing)

- [ ] **Step 1: Build and install on the test phone**

```bash
flutter run --release -d <phone-id>
```

- [ ] **Step 2: Walk the happy path**

Step through and tick:

```
- [ ] After Adopt, app lands on Control screen with the lamp's brightness/colors loaded.
- [ ] Brightness slider visibly changes the lamp's brightness as you drag.
- [ ] Releasing the slider leaves the lamp at the released value.
- [ ] Tapping Shade opens the color picker; picking a color and Save updates the lamp.
- [ ] Tapping Base opens the editor sheet; picking a stop color updates the lamp live.
- [ ] Adding a stop, then reordering it, doesn't crash; lamp reflects the new gradient.
- [ ] Removing a stop down to 1 disables the ✕ on the last remaining stop.
- [ ] Backing out of the lamp and re-entering reads the lamp's current state (no stale data).
```

- [ ] **Step 3: Failure modes**

Tick or file a follow-up:

```
- [ ] If the lamp is off / out of range, the screen shows the error message gracefully (not a stack trace).
- [ ] Wrong password on an adopted lamp: writes silently no-op on the firmware side; UI still appears to work until you notice nothing changed on the lamp. Decide whether to surface a follow-up read-after-write check in a later phase.
```

- [ ] **Step 4: Commit the filled-in checklist**

```bash
git add test/manual/v1-checklist.md
git commit -m "test(control): manual hardware checklist for Phase 2"
```

---

## Self-Review notes for the executor

- All section reads go through `BleClient.read` (already implemented). InMemoryBleClient's `read` throws `BleNotFound` for a key it hasn't seen — that's why the notifier test pre-seeds via `write()`.
- `flutter_colorpicker` doesn't need its own theming hook for dark — by default it renders fine inside a dark `Scaffold` because the sat/val square + hue strip are color-driven. If a follow-up reviewer wants a tighter brand look, swap in a custom `CustomPainter` hue ring (spec §9 / §15.1).
- `setBaseAc` is local-only in this phase. The firmware reads `ac` from the base settings blob (`CHAR_SETTINGS_BLOB`) on full save, which Phase 5 (Setup screen) owns. If users complain that the active dot doesn't survive a power cycle, promote `ac` to its own write or save the base settings blob from Control — both are minor follow-ups.
- The Riverpod family key is the device id. Switching the active lamp tears down the previous notifier (`keepAlive: false`), which fires `onDispose` → flush coalescers + disconnect.
