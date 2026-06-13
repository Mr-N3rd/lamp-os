# Flutter Rewrite — Phase 1b (AddLamp wizard + adopt) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Land the AddLamp flow on top of Phase 1a's BLE plumbing. Two paths: **claim** (unconfigured lamp → 4-step wizard writes name + WiFi) and **adopt** (already-configured lamp → one-tap add to inventory). End state: tapping "+ Add a lamp" on the empty inventory screen walks the user to a working lamp shell at `/lamp/:id/control`.

**Architecture:** A `ConnectionNotifier` family (per-device id) drives connect/disconnect via `BleClient`. A `SetupClient` is a thin task-flow helper that wraps the setup-service writes (SSID, PWD, Name, Apply). An `AddLampNotifier` owns the wizard state (Freezed `AddLampState`) and orchestrates the four UI steps; the steps are dumb widgets reading + dispatching against that notifier. The "adopt" branch shortcuts steps 2-4 and just writes inventory.

**Tech Stack:** flutter_blue_plus (connect/disconnect), existing Riverpod 4 + Freezed 3 + go_router 17 setup.

**Spec:** [docs/superpowers/specs/2026-05-28-flutter-rewrite-design.md](../specs/2026-05-28-flutter-rewrite-design.md) §8.9, §10.

**Prerequisite:** Phase 1a foundation is complete (BLE scan + nearby lamps notifier work on real hardware).

**Phases after this one:** 1c — lamp picker bottom sheet + status-dot wiring; then 2-5 per spec.

---

## File Structure (this plan creates / modifies)

```
software/lamp-app-flutter/lib/
├── core/ble/
│   ├── ble_client.dart                     # T1 — add connect/disconnect
│   ├── fbp_ble_client.dart                 # T1 — impl
│   ├── connection_notifier.dart            # T2 — Riverpod family
│   └── setup_client.dart                   # T3 — high-level setup writes
└── features/
    ├── onboarding/
    │   ├── domain/
    │   │   └── add_lamp_state.dart         # T4 — Freezed wizard state
    │   ├── application/
    │   │   └── add_lamp_notifier.dart      # T5 — wizard orchestrator
    │   └── presentation/
    │       ├── add_lamp_shell.dart         # T10 — PageView + progress dots
    │       ├── widgets/
    │       │   ├── add_lamp_scan_step.dart # T6
    │       │   ├── add_lamp_name_step.dart # T7
    │       │   ├── add_lamp_wifi_step.dart # T8
    │       │   └── add_lamp_done_step.dart # T9
    │       └── onboarding_placeholder.dart # T10 — "+ Add a lamp" button hooks up
```

Tests live next to source in `test/...` mirroring the structure. UI tests for step widgets use `pumpWidget` with a fake notifier override. The connect path can't be unit-tested against the real driver; verified manually in T11 on the phone.

---

## Task 1: Extend BleClient with connect/disconnect

**Files:**
- Modify: `software/lamp-app-flutter/lib/core/ble/ble_client.dart`
- Modify: `software/lamp-app-flutter/lib/core/ble/fbp_ble_client.dart`
- Modify: `software/lamp-app-flutter/test/core/ble/ble_client_test.dart` (add connect tests)

- [ ] **Step 1: Failing test additions**

Add to `software/lamp-app-flutter/test/core/ble/ble_client_test.dart`, after the existing tests:

```dart
  test('InMemoryBleClient tracks connect/disconnect', () async {
    final ble = InMemoryBleClient();
    expect(ble.isConnected('dev1'), isFalse);
    await ble.connect('dev1');
    expect(ble.isConnected('dev1'), isTrue);
    await ble.disconnect('dev1');
    expect(ble.isConnected('dev1'), isFalse);
  });

  test('write throws BleNotConnected on a disconnected device', () async {
    final ble = InMemoryBleClient();
    expect(
      () => ble.write('dev1', 'svc', 'chr', Uint8List.fromList([1])),
      throwsA(isA<BleNotConnected>()),
    );
  });

  test('write succeeds after connect', () async {
    final ble = InMemoryBleClient();
    await ble.connect('dev1');
    await ble.write('dev1', 'svc', 'chr', Uint8List.fromList([1]));
    expect(await ble.read('dev1', 'svc', 'chr'), Uint8List.fromList([1]));
  });
```

- [ ] **Step 2: Run, expect failure**

```bash
cd software/lamp-app-flutter && flutter test test/core/ble/ble_client_test.dart
```

- [ ] **Step 3: Edit `ble_client.dart`**

Add the new exception:

```dart
class BleNotConnected implements Exception {
  const BleNotConnected(this.deviceId);
  final String deviceId;
  @override
  String toString() => 'BleNotConnected: $deviceId';
}
```

Extend the `BleClient` interface:

```dart
abstract class BleClient {
  Future<void> connect(String deviceId);
  Future<void> disconnect(String deviceId);
  bool isConnected(String deviceId);
  Future<Uint8List> read(String deviceId, String serviceUuid, String charUuid);
  Future<void> write(
      String deviceId, String serviceUuid, String charUuid, Uint8List value);
  Stream<Uint8List> subscribe(
      String deviceId, String serviceUuid, String charUuid);
}
```

In `InMemoryBleClient`, add a `Set<String> _connected = {};` field, implement `connect`/`disconnect`/`isConnected`, and **change** `read` and `write` to check connection status:

```dart
final Set<String> _connected = {};

@override
Future<void> connect(String deviceId) async {
  _connected.add(deviceId);
}

@override
Future<void> disconnect(String deviceId) async {
  _connected.remove(deviceId);
}

@override
bool isConnected(String deviceId) => _connected.contains(deviceId);

// In read:
Future<Uint8List> read(String d, String s, String c) async {
  if (!_connected.contains(d)) throw BleNotConnected(d);
  ...
}

// In write — keep the encryption-failure check, but check connection FIRST:
Future<void> write(String d, String s, String c, Uint8List v) async {
  if (!_connected.contains(d)) throw BleNotConnected(d);
  ...
}
```

- [ ] **Step 4: Edit `fbp_ble_client.dart`**

Add `connect`/`disconnect`/`isConnected`:

```dart
@override
Future<void> connect(String deviceId) async {
  final device = fbp.BluetoothDevice(remoteId: fbp.DeviceIdentifier(deviceId));
  await device.connect(autoConnect: false, mtu: 247);
}

@override
Future<void> disconnect(String deviceId) async {
  final device = fbp.BluetoothDevice(remoteId: fbp.DeviceIdentifier(deviceId));
  await device.disconnect();
}

@override
bool isConnected(String deviceId) {
  return fbp.FlutterBluePlus.connectedDevices.any(
    (d) => d.remoteId.str == deviceId,
  );
}
```

If `BluetoothDevice` doesn't have a public constructor (depends on fbp version) or `mtu` named arg doesn't exist, use the closest equivalent; behavior to preserve is "connect to the device by id with a reasonable MTU".

- [ ] **Step 5: Run tests, expect pass**

```bash
flutter test test/core/ble/ble_client_test.dart
flutter test                    # confirm no regressions
flutter analyze
```

- [ ] **Step 6: Commit**

```bash
cd /Users/jerrett/projects/lamp-os/.claude/worktrees/flutter-rewrite
git add software/lamp-app-flutter/lib/core/ble/ble_client.dart \
        software/lamp-app-flutter/lib/core/ble/fbp_ble_client.dart \
        software/lamp-app-flutter/test/core/ble/ble_client_test.dart
git commit -m "$(cat <<'EOF'
feat(ble): add connect/disconnect/isConnected to BleClient

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: ConnectionNotifier

**Files:**
- Create: `software/lamp-app-flutter/lib/core/ble/connection_notifier.dart`
- Create: `software/lamp-app-flutter/test/core/ble/connection_notifier_test.dart`

The notifier exposes `ConnectionStatus` (`disconnected | connecting | connected | error`) per device id. UI watches it to gate buttons + show progress.

- [ ] **Step 1: Failing test**

```dart
// test/core/ble/connection_notifier_test.dart
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/connection_notifier.dart';

void main() {
  test('connect transitions disconnected → connecting → connected', () async {
    final ble = InMemoryBleClient();
    final container = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(container.dispose);

    expect(container.read(connectionNotifierProvider('dev1')),
        ConnectionStatus.disconnected);

    final future =
        container.read(connectionNotifierProvider('dev1').notifier).connect();
    expect(container.read(connectionNotifierProvider('dev1')),
        ConnectionStatus.connecting);

    await future;
    expect(container.read(connectionNotifierProvider('dev1')),
        ConnectionStatus.connected);
  });

  test('disconnect transitions connected → disconnected', () async {
    final ble = InMemoryBleClient();
    final container = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(container.dispose);
    await container.read(connectionNotifierProvider('dev1').notifier).connect();
    await container
        .read(connectionNotifierProvider('dev1').notifier)
        .disconnect();
    expect(container.read(connectionNotifierProvider('dev1')),
        ConnectionStatus.disconnected);
  });
}
```

- [ ] **Step 2: Run, expect fail**

```bash
flutter test test/core/ble/connection_notifier_test.dart
```

- [ ] **Step 3: Create the notifier**

```dart
// lib/core/ble/connection_notifier.dart
import 'package:riverpod_annotation/riverpod_annotation.dart';

import 'ble_client_provider.dart';

part 'connection_notifier.g.dart';

enum ConnectionStatus { disconnected, connecting, connected, error }

@Riverpod(keepAlive: true, name: 'connectionNotifierProvider')
class ConnectionNotifier extends _$ConnectionNotifier {
  @override
  ConnectionStatus build(String deviceId) => ConnectionStatus.disconnected;

  Future<void> connect() async {
    state = ConnectionStatus.connecting;
    try {
      await ref.read(bleClientProvider).connect(deviceId);
      state = ConnectionStatus.connected;
    } catch (_) {
      state = ConnectionStatus.error;
      rethrow;
    }
  }

  Future<void> disconnect() async {
    try {
      await ref.read(bleClientProvider).disconnect(deviceId);
    } finally {
      state = ConnectionStatus.disconnected;
    }
  }
}
```

- [ ] **Step 4: Codegen**

```bash
dart run build_runner build --delete-conflicting-outputs
```

- [ ] **Step 5: Run, expect pass**

```bash
flutter test test/core/ble/connection_notifier_test.dart
```

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app-flutter/lib/core/ble/connection_notifier.dart \
        software/lamp-app-flutter/lib/core/ble/connection_notifier.g.dart \
        software/lamp-app-flutter/test/core/ble/connection_notifier_test.dart
git commit -m "$(cat <<'EOF'
feat(ble): ConnectionNotifier — per-device connect/disconnect state

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: SetupClient — high-level setup-service writes

**Files:**
- Create: `software/lamp-app-flutter/lib/core/ble/setup_client.dart`
- Create: `software/lamp-app-flutter/test/core/ble/setup_client_test.dart`

Thin convenience layer on top of BleClient for the AddLamp claim flow. Encapsulates "for these creds, write SSID + PWD + Name to the setup characteristics, then poke Apply".

- [ ] **Step 1: Failing test**

```dart
// test/core/ble/setup_client_test.dart
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/setup_client.dart';
import 'package:lamp_app/core/ble/uuids.dart';

void main() {
  test('claim writes SSID, password, name, then triggers apply', () async {
    final ble = InMemoryBleClient();
    await ble.connect('dev1');
    final setup = SetupClient(ble: ble);

    await setup.claim(
      deviceId: 'dev1',
      name: 'jacko',
      ssid: 'lamplit-wifi',
      password: 'secret',
    );

    expect(
      utf8.decode(await ble.read('dev1', BleUuids.setupService, BleUuids.setupSsid)),
      'lamplit-wifi',
    );
    expect(
      utf8.decode(await ble.read('dev1', BleUuids.setupService, BleUuids.setupPwd)),
      'secret',
    );
    expect(
      utf8.decode(await ble.read('dev1', BleUuids.setupService, BleUuids.setupName)),
      'jacko',
    );
    expect(
      await ble.read('dev1', BleUuids.setupService, BleUuids.setupApply),
      Uint8List.fromList([0x01]),
    );
  });
}
```

- [ ] **Step 2: Run, expect fail**

```bash
flutter test test/core/ble/setup_client_test.dart
```

- [ ] **Step 3: Create the client**

```dart
// lib/core/ble/setup_client.dart
import 'dart:convert';
import 'dart:typed_data';

import 'ble_client.dart';
import 'uuids.dart';

class SetupClient {
  SetupClient({required this.ble});

  final BleClient ble;

  Future<void> claim({
    required String deviceId,
    required String name,
    required String ssid,
    required String password,
  }) async {
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupSsid,
        Uint8List.fromList(utf8.encode(ssid)));
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupPwd,
        Uint8List.fromList(utf8.encode(password)));
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupName,
        Uint8List.fromList(utf8.encode(name)));
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupApply,
        Uint8List.fromList([0x01]));
  }
}
```

- [ ] **Step 4: Run, expect pass**

```bash
flutter test test/core/ble/setup_client_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add software/lamp-app-flutter/lib/core/ble/setup_client.dart \
        software/lamp-app-flutter/test/core/ble/setup_client_test.dart
git commit -m "$(cat <<'EOF'
feat(ble): SetupClient — wraps setup-service writes for the claim flow

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: AddLampState Freezed model

**Files:**
- Create: `software/lamp-app-flutter/lib/features/onboarding/domain/add_lamp_state.dart`
- Create: `software/lamp-app-flutter/test/features/onboarding/add_lamp_state_test.dart`

- [ ] **Step 1: Failing test**

```dart
// test/features/onboarding/add_lamp_state_test.dart
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/onboarding/domain/add_lamp_state.dart';

void main() {
  test('initial state is at scan step with empty fields', () {
    const s = AddLampState();
    expect(s.step, AddLampStep.scan);
    expect(s.deviceId, '');
    expect(s.name, '');
    expect(s.ssid, '');
    expect(s.password, '');
    expect(s.status, AddLampStatus.idle);
  });

  test('copyWith advances step', () {
    const s = AddLampState();
    final next = s.copyWith(step: AddLampStep.name, deviceId: 'aa');
    expect(next.step, AddLampStep.name);
    expect(next.deviceId, 'aa');
    expect(next.name, '');
  });
}
```

- [ ] **Step 2: Run, expect fail**

```bash
flutter test test/features/onboarding/add_lamp_state_test.dart
```

- [ ] **Step 3: Create the model**

```dart
// lib/features/onboarding/domain/add_lamp_state.dart
import 'package:freezed_annotation/freezed_annotation.dart';

part 'add_lamp_state.freezed.dart';
part 'add_lamp_state.g.dart';

enum AddLampStep { scan, name, wifi, done }

enum AddLampStatus { idle, working, error }

@freezed
abstract class AddLampState with _$AddLampState {
  const factory AddLampState({
    @Default(AddLampStep.scan) AddLampStep step,
    @Default('') String deviceId,
    @Default('') String name,
    @Default('') String ssid,
    @Default('') String password,
    @Default(AddLampStatus.idle) AddLampStatus status,
    String? errorMessage,
  }) = _AddLampState;

  factory AddLampState.fromJson(Map<String, dynamic> json) =>
      _$AddLampStateFromJson(json);
}
```

- [ ] **Step 4: Codegen**

```bash
dart run build_runner build --delete-conflicting-outputs
```

- [ ] **Step 5: Run, expect pass**

```bash
flutter test test/features/onboarding/add_lamp_state_test.dart
```

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app-flutter/lib/features/onboarding/domain \
        software/lamp-app-flutter/test/features/onboarding/add_lamp_state_test.dart
git commit -m "$(cat <<'EOF'
feat(onboarding): AddLampState Freezed model (step + form fields + status)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: AddLampNotifier — wizard orchestrator

**Files:**
- Create: `software/lamp-app-flutter/lib/features/onboarding/application/add_lamp_notifier.dart`
- Create: `software/lamp-app-flutter/test/features/onboarding/add_lamp_notifier_test.dart`

Drives the wizard: select(deviceId), setName, setSsid, setPassword, next(), previous(), submit().

- [ ] **Step 1: Failing test**

```dart
// test/features/onboarding/add_lamp_notifier_test.dart
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/features/inventory/application/active_lamp_notifier.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/onboarding/application/add_lamp_notifier.dart';
import 'package:lamp_app/features/onboarding/domain/add_lamp_state.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  test('select(deviceId) sets the id and advances to name step', () async {
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);
    await c.read(addLampNotifierProvider.notifier).select('dev1');
    final s = c.read(addLampNotifierProvider);
    expect(s.deviceId, 'dev1');
    expect(s.step, AddLampStep.name);
  });

  test('setName, setSsid, setPassword update fields', () {
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);
    final n = c.read(addLampNotifierProvider.notifier);
    n.setName('jacko');
    n.setSsid('home');
    n.setPassword('secret');
    final s = c.read(addLampNotifierProvider);
    expect(s.name, 'jacko');
    expect(s.ssid, 'home');
    expect(s.password, 'secret');
  });

  test('submit() runs claim then adds to inventory + sets active', () async {
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);
    // Prime inventory async load.
    await c.read(inventoryNotifierProvider.future);
    await c.read(activeLampNotifierProvider.future);

    final n = c.read(addLampNotifierProvider.notifier);
    await n.select('dev1');
    n.setName('jacko');
    n.setSsid('home');
    n.setPassword('secret');
    await n.submit();

    final s = c.read(addLampNotifierProvider);
    expect(s.step, AddLampStep.done);
    expect(s.status, AddLampStatus.idle);

    final inventory = await c.read(inventoryNotifierProvider.future);
    expect(inventory.map((l) => l.id).toList(), ['dev1']);

    final active = await c.read(activeLampNotifierProvider.future);
    expect(active, 'dev1');
  });

  test('adopt(deviceId, name) skips wizard and adds to inventory', () async {
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);
    await c.read(inventoryNotifierProvider.future);
    await c.read(activeLampNotifierProvider.future);

    await c
        .read(addLampNotifierProvider.notifier)
        .adopt(deviceId: 'dev2', name: 'melonie');

    final inv = await c.read(inventoryNotifierProvider.future);
    expect(inv.map((l) => l.name).toList(), ['melonie']);

    final active = await c.read(activeLampNotifierProvider.future);
    expect(active, 'dev2');
  });
}
```

- [ ] **Step 2: Run, expect fail**

```bash
flutter test test/features/onboarding/add_lamp_notifier_test.dart
```

- [ ] **Step 3: Create the notifier**

```dart
// lib/features/onboarding/application/add_lamp_notifier.dart
import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/setup_client.dart';
import '../../inventory/application/active_lamp_notifier.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../../inventory/domain/inventory_lamp.dart';
import '../domain/add_lamp_state.dart';

part 'add_lamp_notifier.g.dart';

@Riverpod(keepAlive: true, name: 'addLampNotifierProvider')
class AddLampNotifier extends _$AddLampNotifier {
  @override
  AddLampState build() => const AddLampState();

  Future<void> select(String deviceId) async {
    final ble = ref.read(bleClientProvider);
    await ble.connect(deviceId);
    state = state.copyWith(
      deviceId: deviceId,
      step: AddLampStep.name,
    );
  }

  void setName(String n) => state = state.copyWith(name: n);
  void setSsid(String s) => state = state.copyWith(ssid: s);
  void setPassword(String p) => state = state.copyWith(password: p);

  void next() {
    state = state.copyWith(step: switch (state.step) {
      AddLampStep.scan => AddLampStep.name,
      AddLampStep.name => AddLampStep.wifi,
      AddLampStep.wifi => AddLampStep.done,
      AddLampStep.done => AddLampStep.done,
    });
  }

  void previous() {
    state = state.copyWith(step: switch (state.step) {
      AddLampStep.scan => AddLampStep.scan,
      AddLampStep.name => AddLampStep.scan,
      AddLampStep.wifi => AddLampStep.name,
      AddLampStep.done => AddLampStep.wifi,
    });
  }

  Future<void> submit() async {
    state = state.copyWith(status: AddLampStatus.working, errorMessage: null);
    try {
      final ble = ref.read(bleClientProvider);
      await SetupClient(ble: ble).claim(
        deviceId: state.deviceId,
        name: state.name,
        ssid: state.ssid,
        password: state.password,
      );
      await ref.read(inventoryNotifierProvider.notifier).add(
            InventoryLamp(id: state.deviceId, name: state.name),
          );
      await ref
          .read(activeLampNotifierProvider.notifier)
          .set(state.deviceId);
      state = state.copyWith(
        step: AddLampStep.done,
        status: AddLampStatus.idle,
      );
    } catch (e) {
      state = state.copyWith(
        status: AddLampStatus.error,
        errorMessage: e.toString(),
      );
    }
  }

  Future<void> adopt({
    required String deviceId,
    required String name,
  }) async {
    state = state.copyWith(status: AddLampStatus.working, errorMessage: null);
    try {
      await ref.read(inventoryNotifierProvider.notifier).add(
            InventoryLamp(id: deviceId, name: name),
          );
      await ref.read(activeLampNotifierProvider.notifier).set(deviceId);
      state = state.copyWith(
        deviceId: deviceId,
        name: name,
        step: AddLampStep.done,
        status: AddLampStatus.idle,
      );
    } catch (e) {
      state = state.copyWith(
        status: AddLampStatus.error,
        errorMessage: e.toString(),
      );
    }
  }

  void reset() => state = const AddLampState();
}
```

- [ ] **Step 4: Codegen**

```bash
dart run build_runner build --delete-conflicting-outputs
```

- [ ] **Step 5: Run, expect 4 tests pass**

```bash
flutter test test/features/onboarding/add_lamp_notifier_test.dart
```

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app-flutter/lib/features/onboarding/application \
        software/lamp-app-flutter/test/features/onboarding/add_lamp_notifier_test.dart
git commit -m "$(cat <<'EOF'
feat(onboarding): AddLampNotifier — wizard state machine + claim/adopt actions

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: AddLampScanStep widget

**Files:** Create `software/lamp-app-flutter/lib/features/onboarding/presentation/widgets/add_lamp_scan_step.dart`.

A scrollable list of nearby lamps from `nearbyLampsNotifierProvider`. Each row shows name, id snippet, rssi, and a "configured" / "unconfigured" pill. Tapping an **unconfigured** lamp calls `notifier.select(id)` (advances to name step). Tapping a **configured** lamp opens a small dialog: "Adopt this lamp?" with a name field defaulting to the advertised name; OK calls `notifier.adopt(id, name)`.

- [ ] **Step 1: Create the widget**

```dart
// lib/features/onboarding/presentation/widgets/add_lamp_scan_step.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../../nearby/application/nearby_lamps_notifier.dart';
import '../../../nearby/domain/nearby_lamp.dart';
import '../../application/add_lamp_notifier.dart';

class AddLampScanStep extends ConsumerWidget {
  const AddLampScanStep({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final lamps = ref.watch(nearbyLampsNotifierProvider);
    if (lamps.isEmpty) {
      return const Center(
        child: Text(
          'Scanning for lamps...',
          style: TextStyle(color: BrandColors.fogGrey),
        ),
      );
    }
    return ListView.separated(
      padding: const EdgeInsets.all(16),
      itemCount: lamps.length,
      separatorBuilder: (_, _) => const SizedBox(height: 8),
      itemBuilder: (context, i) => _LampRow(lamp: lamps[i]),
    );
  }
}

class _LampRow extends ConsumerWidget {
  const _LampRow({required this.lamp});
  final NearbyLamp lamp;

  Future<void> _onTap(BuildContext context, WidgetRef ref) async {
    if (lamp.isConfigured) {
      final name = await _askAdoptName(context, lamp.name);
      if (name != null) {
        await ref
            .read(addLampNotifierProvider.notifier)
            .adopt(deviceId: lamp.id, name: name);
      }
    } else {
      await ref.read(addLampNotifierProvider.notifier).select(lamp.id);
    }
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    return InkWell(
      borderRadius: BorderRadius.circular(12),
      onTap: () => _onTap(context, ref),
      child: Container(
        padding: const EdgeInsets.all(12),
        decoration: BoxDecoration(
          color: Colors.white.withValues(alpha: 0.04),
          borderRadius: BorderRadius.circular(12),
          border:
              Border.all(color: Colors.white.withValues(alpha: 0.06)),
        ),
        child: Row(
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    lamp.name.isEmpty ? '(unnamed)' : lamp.name,
                    style: const TextStyle(
                      color: BrandColors.lampWhite,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  Text(
                    '${lamp.id} · ${lamp.rssi} dBm',
                    style: const TextStyle(
                      color: BrandColors.slateGrey,
                      fontSize: 11,
                    ),
                  ),
                ],
              ),
            ),
            _Pill(configured: lamp.isConfigured),
          ],
        ),
      ),
    );
  }
}

class _Pill extends StatelessWidget {
  const _Pill({required this.configured});
  final bool configured;

  @override
  Widget build(BuildContext context) {
    final base =
        configured ? BrandColors.lumenGreen : BrandColors.amberGold;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(999),
        color: base.withValues(alpha: 0.18),
      ),
      child: Text(
        configured ? 'adopt' : 'set up',
        style: TextStyle(
          fontSize: 10,
          color: base,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }
}

Future<String?> _askAdoptName(BuildContext context, String suggested) async {
  final controller = TextEditingController(text: suggested);
  return showDialog<String>(
    context: context,
    builder: (ctx) => AlertDialog(
      title: const Text('Adopt this lamp?'),
      content: TextField(
        controller: controller,
        autofocus: true,
        decoration: const InputDecoration(labelText: 'Name'),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(ctx),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: () => Navigator.pop(ctx, controller.text.trim()),
          child: const Text('Adopt'),
        ),
      ],
    ),
  );
}
```

- [ ] **Step 2: Analyze**

```bash
flutter analyze lib/features/onboarding/presentation/widgets/add_lamp_scan_step.dart
```

- [ ] **Step 3: Commit**

```bash
git add software/lamp-app-flutter/lib/features/onboarding/presentation/widgets/add_lamp_scan_step.dart
git commit -m "$(cat <<'EOF'
feat(onboarding): AddLampScanStep — list nearby lamps, claim or adopt on tap

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: AddLampNameStep widget

**Files:** Create `software/lamp-app-flutter/lib/features/onboarding/presentation/widgets/add_lamp_name_step.dart`.

Single text field for lamp name, with a row of 3 suggestion chips ("jacko", "melonie", "bowery"). Continue is enabled when the name field is non-empty.

- [ ] **Step 1: Create the widget**

```dart
// lib/features/onboarding/presentation/widgets/add_lamp_name_step.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../application/add_lamp_notifier.dart';

const _suggestions = ['jacko', 'melonie', 'bowery'];

class AddLampNameStep extends ConsumerStatefulWidget {
  const AddLampNameStep({super.key});

  @override
  ConsumerState<AddLampNameStep> createState() => _AddLampNameStepState();
}

class _AddLampNameStepState extends ConsumerState<AddLampNameStep> {
  late final TextEditingController _controller =
      TextEditingController(text: ref.read(addLampNotifierProvider).name);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final notifier = ref.read(addLampNotifierProvider.notifier);
    final state = ref.watch(addLampNotifierProvider);
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'Name this lamp',
            style: TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 18,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _controller,
            autofocus: true,
            onChanged: notifier.setName,
            decoration: const InputDecoration(
              labelText: 'Lamp name',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 16),
          Wrap(
            spacing: 8,
            children: _suggestions.map((s) {
              return ActionChip(
                label: Text(s),
                onPressed: () {
                  _controller.text = s;
                  notifier.setName(s);
                },
              );
            }).toList(),
          ),
          const Spacer(),
          Row(
            children: [
              TextButton(
                onPressed: notifier.previous,
                child: const Text('Back'),
              ),
              const Spacer(),
              FilledButton(
                onPressed: state.name.isEmpty ? null : notifier.next,
                child: const Text('Continue'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
```

- [ ] **Step 2: Analyze + commit**

```bash
flutter analyze lib/features/onboarding/presentation/widgets/add_lamp_name_step.dart
git add software/lamp-app-flutter/lib/features/onboarding/presentation/widgets/add_lamp_name_step.dart
git commit -m "$(cat <<'EOF'
feat(onboarding): AddLampNameStep widget with suggestion chips

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: AddLampWifiStep widget

**Files:** Create `software/lamp-app-flutter/lib/features/onboarding/presentation/widgets/add_lamp_wifi_step.dart`.

Two fields (SSID + password, password obscured). Continue enabled when both non-empty.

- [ ] **Step 1: Create the widget**

```dart
// lib/features/onboarding/presentation/widgets/add_lamp_wifi_step.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../application/add_lamp_notifier.dart';

class AddLampWifiStep extends ConsumerStatefulWidget {
  const AddLampWifiStep({super.key});

  @override
  ConsumerState<AddLampWifiStep> createState() => _AddLampWifiStepState();
}

class _AddLampWifiStepState extends ConsumerState<AddLampWifiStep> {
  late final TextEditingController _ssid =
      TextEditingController(text: ref.read(addLampNotifierProvider).ssid);
  late final TextEditingController _pwd =
      TextEditingController(text: ref.read(addLampNotifierProvider).password);

  @override
  void dispose() {
    _ssid.dispose();
    _pwd.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final notifier = ref.read(addLampNotifierProvider.notifier);
    final state = ref.watch(addLampNotifierProvider);
    final canContinue = state.ssid.isNotEmpty && state.password.isNotEmpty;
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'Home WiFi',
            style: TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 18,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            'The lamp connects to your home WiFi so other lamps can find it.',
            style: TextStyle(color: BrandColors.fogGrey),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _ssid,
            autofocus: true,
            onChanged: notifier.setSsid,
            decoration: const InputDecoration(
              labelText: 'Network name (SSID)',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _pwd,
            obscureText: true,
            onChanged: notifier.setPassword,
            decoration: const InputDecoration(
              labelText: 'Password',
              border: OutlineInputBorder(),
            ),
          ),
          const Spacer(),
          if (state.status == AddLampStatus.error)
            Padding(
              padding: const EdgeInsets.only(bottom: 12),
              child: Text(
                state.errorMessage ?? 'Setup failed',
                style: const TextStyle(color: Colors.redAccent),
              ),
            ),
          Row(
            children: [
              TextButton(
                onPressed: state.status == AddLampStatus.working
                    ? null
                    : notifier.previous,
                child: const Text('Back'),
              ),
              const Spacer(),
              FilledButton(
                onPressed: (canContinue &&
                        state.status != AddLampStatus.working)
                    ? notifier.submit
                    : null,
                child: state.status == AddLampStatus.working
                    ? const SizedBox(
                        width: 16,
                        height: 16,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Text('Set up lamp'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
```

- [ ] **Step 2: Analyze + commit**

```bash
flutter analyze lib/features/onboarding/presentation/widgets/add_lamp_wifi_step.dart
git add software/lamp-app-flutter/lib/features/onboarding/presentation/widgets/add_lamp_wifi_step.dart
git commit -m "$(cat <<'EOF'
feat(onboarding): AddLampWifiStep widget (SSID + password + submit)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: AddLampDoneStep widget

**Files:** Create `software/lamp-app-flutter/lib/features/onboarding/presentation/widgets/add_lamp_done_step.dart`.

- [ ] **Step 1: Create the widget**

```dart
// lib/features/onboarding/presentation/widgets/add_lamp_done_step.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../../core/routing/routes.dart';
import '../../../../core/theme/brand_colors.dart';
import '../../application/add_lamp_notifier.dart';

class AddLampDoneStep extends ConsumerWidget {
  const AddLampDoneStep({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final state = ref.watch(addLampNotifierProvider);
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.center,
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const Icon(
            Icons.check_circle,
            color: BrandColors.lumenGreen,
            size: 64,
          ),
          const SizedBox(height: 16),
          Text(
            '${state.name} is ready',
            style: const TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 22,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            'Your lamp is connected and added to your collection.',
            style: TextStyle(color: BrandColors.fogGrey),
            textAlign: TextAlign.center,
          ),
          const SizedBox(height: 32),
          FilledButton(
            onPressed: () {
              ref.read(addLampNotifierProvider.notifier).reset();
              context.go(AppRoutes.control(state.deviceId));
            },
            child: const Text('Open your lamp'),
          ),
        ],
      ),
    );
  }
}
```

- [ ] **Step 2: Analyze + commit**

```bash
flutter analyze lib/features/onboarding/presentation/widgets/add_lamp_done_step.dart
git add software/lamp-app-flutter/lib/features/onboarding/presentation/widgets/add_lamp_done_step.dart
git commit -m "$(cat <<'EOF'
feat(onboarding): AddLampDoneStep widget — success summary + CTA

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: AddLampShell + route + onboarding entry

**Files:**
- Create: `software/lamp-app-flutter/lib/features/onboarding/presentation/add_lamp_shell.dart`
- Modify: `software/lamp-app-flutter/lib/core/routing/router.dart` (add `/onboarding/add`)
- Modify: `software/lamp-app-flutter/lib/features/onboarding/presentation/onboarding_placeholder.dart` (replace `coming in Phase 1b` copy with a real `+ Add a lamp` button that navigates to `/onboarding/add`)
- Create: `software/lamp-app-flutter/test/features/onboarding/add_lamp_shell_test.dart` (smoke test renders Scan step initially)

- [ ] **Step 1: Create the shell**

```dart
// lib/features/onboarding/presentation/add_lamp_shell.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../application/add_lamp_notifier.dart';
import '../domain/add_lamp_state.dart';
import 'widgets/add_lamp_done_step.dart';
import 'widgets/add_lamp_name_step.dart';
import 'widgets/add_lamp_scan_step.dart';
import 'widgets/add_lamp_wifi_step.dart';

class AddLampShell extends ConsumerWidget {
  const AddLampShell({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final step = ref.watch(addLampNotifierProvider).step;
    final body = switch (step) {
      AddLampStep.scan => const AddLampScanStep(),
      AddLampStep.name => const AddLampNameStep(),
      AddLampStep.wifi => const AddLampWifiStep(),
      AddLampStep.done => const AddLampDoneStep(),
    };
    return Scaffold(
      appBar: AppBar(
        title: const Text('Add a lamp'),
        bottom: PreferredSize(
          preferredSize: const Size.fromHeight(8),
          child: _ProgressDots(currentIndex: step.index),
        ),
      ),
      body: body,
    );
  }
}

class _ProgressDots extends StatelessWidget {
  const _ProgressDots({required this.currentIndex});
  final int currentIndex;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: List.generate(4, (i) {
          final active = i == currentIndex;
          return Container(
            margin: const EdgeInsets.symmetric(horizontal: 4),
            width: active ? 24 : 8,
            height: 8,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(999),
              color: active
                  ? BrandColors.glowPink
                  : BrandColors.slateGrey.withValues(alpha: 0.5),
            ),
          );
        }),
      ),
    );
  }
}
```

- [ ] **Step 2: Add the route**

In `software/lamp-app-flutter/lib/core/routing/router.dart`:

Add import:
```dart
import '../../features/onboarding/presentation/add_lamp_shell.dart';
```

Add route inside `routes: [...]`:
```dart
GoRoute(
  path: '/onboarding/add',
  builder: (_, _) => const AddLampShell(),
),
```

- [ ] **Step 3: Hook up the "+ Add a lamp" button**

Replace `software/lamp-app-flutter/lib/features/onboarding/presentation/onboarding_placeholder.dart` with:

```dart
import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';

class OnboardingPlaceholder extends StatelessWidget {
  const OnboardingPlaceholder({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Text(
              'No lamps yet',
              style: TextStyle(
                color: BrandColors.lampWhite,
                fontWeight: FontWeight.w600,
                fontSize: 18,
              ),
            ),
            const SizedBox(height: 12),
            const Text(
              'Tap below to set up your first lamp.',
              style: TextStyle(color: BrandColors.fogGrey),
            ),
            const SizedBox(height: 24),
            FilledButton.icon(
              onPressed: () => context.go('/onboarding/add'),
              icon: const Icon(Icons.add),
              label: const Text('Add a lamp'),
            ),
            const SizedBox(height: 12),
            TextButton(
              onPressed: () => context.go('/devices'),
              child: const Text('Live scan (debug)'),
            ),
          ],
        ),
      ),
    );
  }
}
```

- [ ] **Step 4: Smoke test for the shell**

```dart
// test/features/onboarding/add_lamp_shell_test.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/features/onboarding/presentation/add_lamp_shell.dart';

void main() {
  testWidgets('AddLampShell renders Scan step initially', (tester) async {
    final ble = InMemoryBleClient();
    await tester.pumpWidget(
      ProviderScope(
        overrides: [bleClientProvider.overrideWithValue(ble)],
        child: const MaterialApp(home: AddLampShell()),
      ),
    );
    await tester.pump();
    expect(find.text('Scanning for lamps...'), findsOneWidget);
  });
}
```

- [ ] **Step 5: Codegen + analyze + full test suite**

```bash
dart run build_runner build --delete-conflicting-outputs
flutter analyze
flutter test
```

Expected: all prior tests + the new tests pass.

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app-flutter/lib/features/onboarding/presentation \
        software/lamp-app-flutter/lib/core/routing/router.dart \
        software/lamp-app-flutter/lib/core/routing/router.g.dart \
        software/lamp-app-flutter/test/features/onboarding/add_lamp_shell_test.dart
git commit -m "$(cat <<'EOF'
feat(onboarding): AddLampShell PageView + route + onboarding entry

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Manual on-device verification

**Files:** none.

- [ ] **Step 1: Install + launch on phone**

From the worktree root:
```bash
npm run app:install
```

- [ ] **Step 2: Run through these checks**

| Check | Pass criterion |
|---|---|
| Empty inventory → onboarding screen | "+ Add a lamp" button visible |
| Tap "+ Add a lamp" | `/onboarding/add` opens, scan step shown, "Scanning for lamps..." |
| Power on a nearby **unconfigured** lamp | It appears with a yellow **set up** pill |
| Tap the unconfigured lamp | App connects (briefly) and advances to Name step |
| Enter "jacko" (or use a suggestion chip) → Continue | Advances to WiFi step |
| Enter SSID + password → "Set up lamp" | Spinner, then Done step with green checkmark + "jacko is ready" |
| Tap "Open your lamp" | Navigates to `/lamp/jacko-id/control` — the (still placeholder) Control surface |
| Back out, tap "+ Add a lamp" again | Wizard restarts at Scan step |
| Now a **configured** lamp appears | Green **adopt** pill |
| Tap configured lamp | Dialog asks for name with the advertised name pre-filled |
| Confirm → Adopt | Skips wizard, goes straight to Done step + "Open your lamp" works |

- [ ] **Step 3: Report findings**

If something fails, note specific symptom + logcat. Otherwise: DONE — Phase 1b complete.

---

## Done — Phase 1b complete

Acceptance:
- `flutter analyze` clean.
- `flutter test` passes at the new test-count (≥45 expected).
- On the phone, the AddLamp + adopt flows produce inventory rows + active-lamp routing.

Branch stays local. **Do not push.**

Next plan: **Phase 1c — lamp picker bottom sheet + status-dot wiring in the lamp shell.**
