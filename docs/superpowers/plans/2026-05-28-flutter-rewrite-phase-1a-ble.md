# Flutter Rewrite — Phase 1a (BLE plumbing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire `flutter_blue_plus` into the existing `BleClient` interface (Phase 0 left the real driver as a TODO behind an in-memory fake), handle Android runtime permissions, expose a scan stream as a `NearbyLamps` Riverpod notifier, and prove it end-to-end with a dev-only Devices screen the user can pull up on their phone.

**Architecture:** Keep the Phase-0 `BleClient` abstract interface as the source of truth — all Riverpod providers continue to depend on it. Add a new `FbpBleClient` (production) that implements it on top of `flutter_blue_plus`. Switch the default `bleClientProvider` to the real driver in production builds; tests still inject `InMemoryBleClient`. Scanning is a separate concern (`BleScanner`) so the read/write/subscribe interface stays small.

**Tech Stack:** flutter_blue_plus (already a dep), permission_handler (new — Android runtime perms), existing Riverpod + Freezed + go_router setup.

**Spec:** [docs/superpowers/specs/2026-05-28-flutter-rewrite-design.md](../specs/2026-05-28-flutter-rewrite-design.md) §6, §7, §8.1

**Prerequisite:** Phase 0 foundation is complete on branch `flutter-rewrite`. Worktree at `/Users/jerrett/projects/lamp-os/.claude/worktrees/flutter-rewrite/`.

**Phases after this one:**
- Phase 1b — Onboarding 4-step wizard + adopt nearby flow
- Phase 1c — Lamp picker bottom sheet + status-dot wiring across the lamp shell
- Phases 2–5 (Control / Knockout / Expressions / Setup) per the spec

---

## File Structure (created in this plan)

```
software/lamp-app-flutter/
├── pubspec.yaml                            # task 1 — add permission_handler
├── lib/
│   ├── core/ble/
│   │   ├── fbp_ble_client.dart             # task 2 — real driver
│   │   ├── ble_client_provider.dart        # task 3 — Riverpod wiring
│   │   ├── ble_permissions.dart            # task 4 — runtime perms wrapper
│   │   └── ble_scanner.dart                # task 5 — scan stream wrapper
│   └── features/
│       ├── nearby/
│       │   ├── domain/
│       │   │   └── nearby_lamp.dart        # task 6 — Freezed model
│       │   ├── application/
│       │   │   └── nearby_lamps_notifier.dart  # task 7 — scan → list
│       │   └── presentation/
│       │       └── nearby_lamps_screen.dart    # task 9 — dev screen
│       └── lamp_shell/presentation/
│           └── (no changes — Phase 1c wires the picker)
└── test/
    ├── core/ble/
    │   ├── ble_permissions_test.dart       # task 4
    │   └── ble_scanner_test.dart           # task 5
    └── features/nearby/
        ├── nearby_lamp_test.dart           # task 6
        └── nearby_lamps_notifier_test.dart # task 7
```

A note on the production driver: there is no good way to TDD `FbpBleClient` (it's a thin glue layer over a plugin that requires hardware to exercise). It's verified manually in Task 10 by running on the phone and watching the Devices screen populate.

---

## Task 1: Add permission_handler dependency

**Files:** Modify `software/lamp-app-flutter/pubspec.yaml`.

- [ ] **Step 1: Add the dependency**

Add to `dependencies:` in `software/lamp-app-flutter/pubspec.yaml`:

```yaml
  permission_handler: ^11.3.1
```

- [ ] **Step 2: Resolve**

```bash
cd software/lamp-app-flutter && flutter pub get
```

If pub bumps to a newer caret-compatible version (as happened in T7 of Phase 0), accept it.

- [ ] **Step 3: Commit**

```bash
git add software/lamp-app-flutter/pubspec.yaml software/lamp-app-flutter/pubspec.lock
git commit -m "$(cat <<'EOF'
chore(deps): add permission_handler for Android BLE runtime perms

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: FbpBleClient — production BleClient backed by flutter_blue_plus

**Files:** Create `software/lamp-app-flutter/lib/core/ble/fbp_ble_client.dart`.

No TDD on this file — see preamble. The task is to translate `BleClient` calls into `flutter_blue_plus` API calls, verified by manual run in Task 10.

- [ ] **Step 1: Create fbp_ble_client.dart**

```dart
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart' as fbp;

import 'ble_client.dart';

/// Production [BleClient] backed by flutter_blue_plus. Each public method
/// resolves the device + service + characteristic from the plugin's live
/// registry, errors out with a friendly exception type when it can't.
class FbpBleClient implements BleClient {
  Future<fbp.BluetoothCharacteristic> _resolve(
    String deviceId,
    String serviceUuid,
    String charUuid,
  ) async {
    final device = fbp.FlutterBluePlus.connectedDevices
        .firstWhere(
          (d) => d.remoteId.str == deviceId,
          orElse: () => throw BleNotFound('device $deviceId not connected'),
        );

    final services = await device.discoverServices();
    final service = services.firstWhere(
      (s) => s.uuid.str128.toLowerCase() == serviceUuid.toLowerCase(),
      orElse: () => throw BleNotFound('service $serviceUuid on $deviceId'),
    );
    final ch = service.characteristics.firstWhere(
      (c) => c.uuid.str128.toLowerCase() == charUuid.toLowerCase(),
      orElse: () => throw BleNotFound('char $charUuid in $serviceUuid'),
    );
    return ch;
  }

  @override
  Future<Uint8List> read(String d, String s, String c) async {
    try {
      final ch = await _resolve(d, s, c);
      final bytes = await ch.read();
      return Uint8List.fromList(bytes);
    } on fbp.FlutterBluePlusException catch (e) {
      if (e.toString().toLowerCase().contains('encryption')) {
        throw BleEncryptionRequired(d);
      }
      rethrow;
    }
  }

  @override
  Future<void> write(String d, String s, String c, Uint8List v) async {
    try {
      final ch = await _resolve(d, s, c);
      await ch.write(v, withoutResponse: false);
    } on fbp.FlutterBluePlusException catch (e) {
      if (e.toString().toLowerCase().contains('encryption')) {
        throw BleEncryptionRequired(d);
      }
      rethrow;
    }
  }

  @override
  Stream<Uint8List> subscribe(String d, String s, String c) async* {
    final ch = await _resolve(d, s, c);
    await ch.setNotifyValue(true);
    yield* ch.lastValueStream.map(Uint8List.fromList);
  }
}
```

- [ ] **Step 2: Verify analyzer is happy**

```bash
flutter analyze lib/core/ble/fbp_ble_client.dart
```
Expected: "No issues found!"

- [ ] **Step 3: Commit**

```bash
git add software/lamp-app-flutter/lib/core/ble/fbp_ble_client.dart
git commit -m "$(cat <<'EOF'
feat(ble): FbpBleClient — production BleClient over flutter_blue_plus

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: bleClientProvider — Riverpod wiring

**Files:**
- Create: `software/lamp-app-flutter/lib/core/ble/ble_client_provider.dart`
- Create: `software/lamp-app-flutter/test/core/ble/ble_client_provider_test.dart`

The provider returns `FbpBleClient` in production but tests override it with `InMemoryBleClient`. We're not testing the production branch (no way to assert "we got an FbpBleClient" usefully); the test asserts the override mechanism works.

- [ ] **Step 1: Failing test**

```dart
// test/core/ble/ble_client_provider_test.dart
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';

void main() {
  test('default provider returns a BleClient instance', () {
    final c = ProviderContainer();
    addTearDown(c.dispose);
    final ble = c.read(bleClientProvider);
    expect(ble, isA<BleClient>());
  });

  test('tests can override with InMemoryBleClient', () {
    final fake = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(fake)],
    );
    addTearDown(c.dispose);
    expect(identical(c.read(bleClientProvider), fake), isTrue);
  });
}
```

- [ ] **Step 2: Verify fail**

```bash
flutter test test/core/ble/ble_client_provider_test.dart
```

- [ ] **Step 3: Create provider**

```dart
// lib/core/ble/ble_client_provider.dart
import 'package:riverpod_annotation/riverpod_annotation.dart';

import 'ble_client.dart';
import 'fbp_ble_client.dart';

part 'ble_client_provider.g.dart';

@Riverpod(keepAlive: true, name: 'bleClientProvider')
BleClient bleClient(Ref ref) => FbpBleClient();
```

- [ ] **Step 4: Codegen**

```bash
dart run build_runner build --delete-conflicting-outputs
```

- [ ] **Step 5: Verify pass**

```bash
flutter test test/core/ble/ble_client_provider_test.dart
```
Expected: 2 tests pass.

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app-flutter/lib/core/ble/ble_client_provider.dart \
        software/lamp-app-flutter/lib/core/ble/ble_client_provider.g.dart \
        software/lamp-app-flutter/test/core/ble/ble_client_provider_test.dart
git commit -m "$(cat <<'EOF'
feat(ble): bleClientProvider wiring (prod → FbpBleClient, tests override)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: BlePermissions — Android runtime permission wrapper

**Files:**
- Create: `software/lamp-app-flutter/lib/core/ble/ble_permissions.dart`
- Create: `software/lamp-app-flutter/test/core/ble/ble_permissions_test.dart`

`permission_handler` is platform-call-based; we wrap it behind a small interface so the notifier (Task 7) can be tested with a fake.

- [ ] **Step 1: Failing test**

```dart
// test/core/ble/ble_permissions_test.dart
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_permissions.dart';

class _AlwaysGranted implements BlePermissions {
  int requestCalls = 0;
  @override
  Future<bool> request() async {
    requestCalls += 1;
    return true;
  }

  @override
  Future<bool> isGranted() async => true;
}

void main() {
  test('test double records request calls', () async {
    final p = _AlwaysGranted();
    expect(await p.isGranted(), isTrue);
    expect(await p.request(), isTrue);
    expect(p.requestCalls, 1);
  });
}
```

- [ ] **Step 2: Verify fail**

```bash
flutter test test/core/ble/ble_permissions_test.dart
```
Expected: FAIL — `BlePermissions` undefined.

- [ ] **Step 3: Create the interface + production impl**

```dart
// lib/core/ble/ble_permissions.dart
import 'dart:io' show Platform;

import 'package:permission_handler/permission_handler.dart';

/// Wraps Android runtime BT permissions. iOS handles BT prompts
/// automatically on first scan/connect, so [request]/[isGranted]
/// both no-op true there.
abstract class BlePermissions {
  Future<bool> isGranted();
  Future<bool> request();
}

class AndroidBlePermissions implements BlePermissions {
  static const _perms = [
    Permission.bluetoothScan,
    Permission.bluetoothConnect,
    Permission.locationWhenInUse,
  ];

  @override
  Future<bool> isGranted() async {
    if (!Platform.isAndroid) return true;
    for (final p in _perms) {
      if (!await p.isGranted) return false;
    }
    return true;
  }

  @override
  Future<bool> request() async {
    if (!Platform.isAndroid) return true;
    final results = await _perms.request();
    return results.values.every((s) => s.isGranted);
  }
}
```

- [ ] **Step 4: Verify pass**

```bash
flutter test test/core/ble/ble_permissions_test.dart
```

- [ ] **Step 5: Commit**

```bash
git add software/lamp-app-flutter/lib/core/ble/ble_permissions.dart \
        software/lamp-app-flutter/test/core/ble/ble_permissions_test.dart
git commit -m "$(cat <<'EOF'
feat(ble): BlePermissions wrapper for Android runtime BT prompts

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: BleScanner — scan stream wrapper

**Files:**
- Create: `software/lamp-app-flutter/lib/core/ble/ble_scanner.dart`
- Create: `software/lamp-app-flutter/test/core/ble/ble_scanner_test.dart`

Scanning sits next to the BleClient interface (different concern — discover vs. connect/read/write). We wrap it behind a `BleScanner` interface so the notifier (Task 7) can be tested with a fake stream.

- [ ] **Step 1: Failing test**

```dart
// test/core/ble/ble_scanner_test.dart
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';

void main() {
  test('FakeBleScanner emits the events sent to it', () async {
    final scanner = FakeBleScanner();
    final emitted = <BleAdvertisement>[];
    final sub = scanner.results().listen(emitted.add);
    scanner.emit(const BleAdvertisement(
      id: 'aa',
      name: 'jacko',
      serviceUuids: ['5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      rssi: -55,
    ));
    scanner.emit(const BleAdvertisement(
      id: 'bb',
      name: 'melonie',
      serviceUuids: ['5f64f4c1-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      rssi: -68,
    ));
    await Future<void>.delayed(Duration.zero);
    await sub.cancel();
    expect(emitted.map((a) => a.name).toList(), ['jacko', 'melonie']);
  });
}
```

- [ ] **Step 2: Verify fail**

```bash
flutter test test/core/ble/ble_scanner_test.dart
```

- [ ] **Step 3: Create scanner interface + production impl + fake**

```dart
// lib/core/ble/ble_scanner.dart
import 'dart:async';

import 'package:flutter_blue_plus/flutter_blue_plus.dart' as fbp;

import 'uuids.dart';

class BleAdvertisement {
  const BleAdvertisement({
    required this.id,
    required this.name,
    required this.serviceUuids,
    required this.rssi,
  });

  final String id;
  final String name;
  final List<String> serviceUuids;
  final int rssi;
}

abstract class BleScanner {
  /// Stream of scan results. Implementations must filter to lamp services
  /// (control + setup) so callers don't see unrelated BLE advertisements.
  Stream<BleAdvertisement> results();

  /// Begin scanning. Must be called once before [results] yields anything
  /// on the real driver. Idempotent — calling twice is a no-op.
  Future<void> start();

  /// Stop scanning (frees the radio).
  Future<void> stop();
}

class FbpBleScanner implements BleScanner {
  StreamSubscription<List<fbp.ScanResult>>? _sub;
  final _ctrl = StreamController<BleAdvertisement>.broadcast();
  bool _running = false;

  @override
  Stream<BleAdvertisement> results() => _ctrl.stream;

  @override
  Future<void> start() async {
    if (_running) return;
    _running = true;
    _sub = fbp.FlutterBluePlus.scanResults.listen((results) {
      for (final r in results) {
        final uuids = r.advertisementData.serviceUuids
            .map((g) => g.str128.toLowerCase())
            .toList();
        const wanted = {BleUuids.controlService, BleUuids.setupService};
        if (!uuids.any(wanted.contains)) continue;
        _ctrl.add(BleAdvertisement(
          id: r.device.remoteId.str,
          name: r.advertisementData.advName.isNotEmpty
              ? r.advertisementData.advName
              : r.device.platformName,
          serviceUuids: uuids,
          rssi: r.rssi,
        ));
      }
    });
    await fbp.FlutterBluePlus.startScan(
      withServices: [
        fbp.Guid(BleUuids.controlService),
        fbp.Guid(BleUuids.setupService),
      ],
      timeout: const Duration(seconds: 10),
      continuousUpdates: true,
    );
  }

  @override
  Future<void> stop() async {
    if (!_running) return;
    _running = false;
    await fbp.FlutterBluePlus.stopScan();
    await _sub?.cancel();
    _sub = null;
  }
}

class FakeBleScanner implements BleScanner {
  final _ctrl = StreamController<BleAdvertisement>.broadcast();
  bool _started = false;

  @override
  Stream<BleAdvertisement> results() => _ctrl.stream;

  @override
  Future<void> start() async {
    _started = true;
  }

  @override
  Future<void> stop() async {
    _started = false;
  }

  void emit(BleAdvertisement ad) {
    if (!_started) {
      throw StateError('scanner not started');
    }
    _ctrl.add(ad);
  }
}
```

Note: the test starts the fake without calling `start()`. Update the test to call `start()` first, OR remove the `StateError` guard from FakeBleScanner. **Choose: keep the guard, update the test** (more accurate to real lifecycle).

- [ ] **Step 4: Update the test to call start() first**

Insert this line in the test, before the first `emit(...)`:

```dart
await scanner.start();
```

- [ ] **Step 5: Verify pass**

```bash
flutter test test/core/ble/ble_scanner_test.dart
```

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app-flutter/lib/core/ble/ble_scanner.dart \
        software/lamp-app-flutter/test/core/ble/ble_scanner_test.dart
git commit -m "$(cat <<'EOF'
feat(ble): BleScanner interface + Fbp + Fake impls (filters to lamp services)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: NearbyLamp Freezed model

**Files:**
- Create: `software/lamp-app-flutter/lib/features/nearby/domain/nearby_lamp.dart`
- Create: `software/lamp-app-flutter/test/features/nearby/nearby_lamp_test.dart`

- [ ] **Step 1: Failing test**

```dart
// test/features/nearby/nearby_lamp_test.dart
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/nearby/domain/nearby_lamp.dart';

void main() {
  test('isConfigured is true when controlService is in serviceUuids', () {
    final lamp = NearbyLamp(
      id: 'aa',
      name: 'jacko',
      rssi: -50,
      serviceUuids: const ['5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      lastSeenEpochMs: 1,
    );
    expect(lamp.isConfigured, isTrue);
    expect(lamp.isUnconfigured, isFalse);
  });

  test('isUnconfigured is true when only setupService is present', () {
    final lamp = NearbyLamp(
      id: 'bb',
      name: '',
      rssi: -70,
      serviceUuids: const ['5f64f4c1-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      lastSeenEpochMs: 1,
    );
    expect(lamp.isUnconfigured, isTrue);
    expect(lamp.isConfigured, isFalse);
  });
}
```

- [ ] **Step 2: Verify fail**

```bash
flutter test test/features/nearby/nearby_lamp_test.dart
```

- [ ] **Step 3: Create the model**

```dart
// lib/features/nearby/domain/nearby_lamp.dart
import 'package:freezed_annotation/freezed_annotation.dart';

import '../../../core/ble/uuids.dart';

part 'nearby_lamp.freezed.dart';
part 'nearby_lamp.g.dart';

@freezed
abstract class NearbyLamp with _$NearbyLamp {
  const NearbyLamp._();

  const factory NearbyLamp({
    required String id,
    required String name,
    required int rssi,
    required List<String> serviceUuids,
    required int lastSeenEpochMs,
  }) = _NearbyLamp;

  factory NearbyLamp.fromJson(Map<String, dynamic> json) =>
      _$NearbyLampFromJson(json);

  bool get isConfigured => serviceUuids
      .map((u) => u.toLowerCase())
      .contains(BleUuids.controlService);

  bool get isUnconfigured => !isConfigured && serviceUuids
      .map((u) => u.toLowerCase())
      .contains(BleUuids.setupService);
}
```

- [ ] **Step 4: Codegen**

```bash
dart run build_runner build --delete-conflicting-outputs
```

- [ ] **Step 5: Verify pass**

```bash
flutter test test/features/nearby/nearby_lamp_test.dart
```

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app-flutter/lib/features/nearby \
        software/lamp-app-flutter/test/features/nearby
git commit -m "$(cat <<'EOF'
feat(nearby): NearbyLamp Freezed model with isConfigured/isUnconfigured

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: NearbyLampsNotifier — scan results → list

**Files:**
- Create: `software/lamp-app-flutter/lib/features/nearby/application/nearby_lamps_notifier.dart`
- Create: `software/lamp-app-flutter/test/features/nearby/nearby_lamps_notifier_test.dart`

The notifier consumes the scan stream, dedupes by id, and evicts entries older than 30 s on each new emission so stale advertisements drop off the list.

- [ ] **Step 1: Failing test**

```dart
// test/features/nearby/nearby_lamps_notifier_test.dart
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';
import 'package:lamp_app/features/nearby/application/nearby_lamps_notifier.dart';

void main() {
  test('emits scan advertisements and dedupes by id', () async {
    final scanner = FakeBleScanner();
    final container = ProviderContainer(
      overrides: [bleScannerProvider.overrideWithValue(scanner)],
    );
    addTearDown(container.dispose);

    // Subscribe so the notifier starts.
    final sub =
        container.listen(nearbyLampsNotifierProvider, (_, __) {});
    addTearDown(sub.close);

    await Future<void>.delayed(const Duration(milliseconds: 10));
    scanner.emit(const BleAdvertisement(
      id: 'aa',
      name: 'jacko',
      serviceUuids: ['5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      rssi: -55,
    ));
    await Future<void>.delayed(const Duration(milliseconds: 10));
    expect(container.read(nearbyLampsNotifierProvider).length, 1);

    // Same id — should replace, not duplicate.
    scanner.emit(const BleAdvertisement(
      id: 'aa',
      name: 'jacko',
      serviceUuids: ['5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      rssi: -50,
    ));
    await Future<void>.delayed(const Duration(milliseconds: 10));
    final lamps = container.read(nearbyLampsNotifierProvider);
    expect(lamps.length, 1);
    expect(lamps.first.rssi, -50);
  });
}
```

- [ ] **Step 2: Verify fail**

```bash
flutter test test/features/nearby/nearby_lamps_notifier_test.dart
```

- [ ] **Step 3: Create the notifier + scanner provider**

```dart
// lib/features/nearby/application/nearby_lamps_notifier.dart
import 'dart:async';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_scanner.dart';
import '../domain/nearby_lamp.dart';

part 'nearby_lamps_notifier.g.dart';

const _staleAfter = Duration(seconds: 30);

@Riverpod(keepAlive: true, name: 'bleScannerProvider')
BleScanner bleScanner(Ref ref) => FbpBleScanner();

@Riverpod(keepAlive: true, name: 'nearbyLampsNotifierProvider')
class NearbyLampsNotifier extends _$NearbyLampsNotifier {
  StreamSubscription<BleAdvertisement>? _sub;

  @override
  List<NearbyLamp> build() {
    final scanner = ref.read(bleScannerProvider);
    scanner.start();
    _sub = scanner.results().listen(_onAd);
    ref.onDispose(() {
      _sub?.cancel();
      scanner.stop();
    });
    return const [];
  }

  void _onAd(BleAdvertisement ad) {
    final now = DateTime.now().millisecondsSinceEpoch;
    final updated = NearbyLamp(
      id: ad.id,
      name: ad.name,
      rssi: ad.rssi,
      serviceUuids: ad.serviceUuids,
      lastSeenEpochMs: now,
    );
    final next = [
      for (final l in state)
        if (l.id != ad.id &&
            now - l.lastSeenEpochMs < _staleAfter.inMilliseconds)
          l,
      updated,
    ];
    state = next;
  }
}
```

- [ ] **Step 4: Codegen**

```bash
dart run build_runner build --delete-conflicting-outputs
```

- [ ] **Step 5: Verify pass**

```bash
flutter test test/features/nearby/nearby_lamps_notifier_test.dart
```

- [ ] **Step 6: Commit**

```bash
git add software/lamp-app-flutter/lib/features/nearby/application \
        software/lamp-app-flutter/test/features/nearby/nearby_lamps_notifier_test.dart
git commit -m "$(cat <<'EOF'
feat(nearby): NearbyLampsNotifier ingests scan stream, dedupes by id, evicts stale

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Permission gate at app boot

**Files:** Modify `software/lamp-app-flutter/lib/app.dart`.

On first app launch, request BT permissions before mounting the router. If the user denies, render a friendly explainer with a Retry button. Cleaner UX than the system surfacing the prompt mid-scan.

- [ ] **Step 1: Replace app.dart**

```dart
// lib/app.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'core/ble/ble_permissions.dart';
import 'core/routing/router.dart';
import 'core/theme/app_theme.dart';
import 'core/theme/brand_colors.dart';

class LampApp extends ConsumerStatefulWidget {
  const LampApp({super.key, BlePermissions? permissions})
      : _injected = permissions;
  final BlePermissions? _injected;

  @override
  ConsumerState<LampApp> createState() => _LampAppState();
}

class _LampAppState extends ConsumerState<LampApp> {
  late final BlePermissions _perms =
      widget._injected ?? AndroidBlePermissions();
  Future<bool>? _granted;

  @override
  void initState() {
    super.initState();
    _granted = _perms.request();
  }

  void _retry() {
    setState(() {
      _granted = _perms.request();
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Lamplit',
      theme: AppTheme.dark(),
      debugShowCheckedModeBanner: false,
      home: FutureBuilder<bool>(
        future: _granted,
        builder: (context, snap) {
          if (snap.connectionState != ConnectionState.done) {
            return const Scaffold(
              body: Center(child: CircularProgressIndicator()),
            );
          }
          if (snap.data != true) {
            return Scaffold(
              body: Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Text(
                      'Bluetooth permission needed',
                      style: TextStyle(
                        color: BrandColors.lampWhite,
                        fontWeight: FontWeight.w600,
                        fontSize: 18,
                      ),
                    ),
                    const SizedBox(height: 12),
                    const Text(
                      'Lamplit talks to your lamps over Bluetooth.',
                      style: TextStyle(color: BrandColors.fogGrey),
                    ),
                    const SizedBox(height: 24),
                    FilledButton(
                      onPressed: _retry,
                      child: const Text('Allow Bluetooth'),
                    ),
                  ],
                ),
              ),
            );
          }
          final router = ref.watch(appRouterProvider);
          return _Router(router: router);
        },
      ),
    );
  }
}

class _Router extends StatelessWidget {
  const _Router({required this.router});
  final dynamic router; // GoRouter, but kept dynamic to avoid extra import

  @override
  Widget build(BuildContext context) {
    return MaterialApp.router(
      title: 'Lamplit',
      theme: AppTheme.dark(),
      routerConfig: router,
      debugShowCheckedModeBanner: false,
    );
  }
}
```

Note the smoke test from Phase 0 (`test/app_smoke_test.dart`) injects a fake permissions object via the `permissions:` constructor parameter so the test doesn't need real Android permissions. Update the smoke test:

```dart
// test/app_smoke_test.dart
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/app.dart';
import 'package:lamp_app/core/ble/ble_permissions.dart';
import 'package:shared_preferences/shared_preferences.dart';

class _AlwaysGranted implements BlePermissions {
  @override
  Future<bool> request() async => true;
  @override
  Future<bool> isGranted() async => true;
}

void main() {
  testWidgets('app boots and shows onboarding when inventory empty',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    await tester.pumpWidget(
      ProviderScope(child: LampApp(permissions: _AlwaysGranted())),
    );
    await tester.pumpAndSettle();
    expect(find.text('No lamps yet'), findsOneWidget);
  });
}
```

- [ ] **Step 2: Run tests, verify nothing regressed**

```bash
flutter test
```
Expected: 30 + 4 new = 34 (or whatever the count is — every prior test plus the new ones added in this phase) passing.

- [ ] **Step 3: Analyze**

```bash
flutter analyze
```

- [ ] **Step 4: Commit**

```bash
git add software/lamp-app-flutter/lib/app.dart \
        software/lamp-app-flutter/test/app_smoke_test.dart
git commit -m "$(cat <<'EOF'
feat(app): permission gate at boot (request BT perms before routing)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Dev-only Devices screen

**Files:**
- Create: `software/lamp-app-flutter/lib/features/nearby/presentation/nearby_lamps_screen.dart`
- Modify: `software/lamp-app-flutter/lib/core/routing/router.dart` (add `/devices` route)

A scrollable list of currently-scanned lamps. Each row: name, id, rssi, "configured" or "unconfigured" pill. Updates live as scan results stream in. Reachable at `/devices` so you can navigate from the onboarding placeholder via a debug button.

- [ ] **Step 1: Create the screen**

```dart
// lib/features/nearby/presentation/nearby_lamps_screen.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../application/nearby_lamps_notifier.dart';

class NearbyLampsScreen extends ConsumerWidget {
  const NearbyLampsScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final lamps = ref.watch(nearbyLampsNotifierProvider);
    return Scaffold(
      appBar: AppBar(title: const Text('Nearby Lamps (debug)')),
      body: lamps.isEmpty
          ? const Center(
              child: Text(
                'Scanning...',
                style: TextStyle(color: BrandColors.fogGrey),
              ),
            )
          : ListView.separated(
              padding: const EdgeInsets.all(16),
              itemCount: lamps.length,
              separatorBuilder: (_, __) => const SizedBox(height: 8),
              itemBuilder: (context, i) {
                final l = lamps[i];
                return Container(
                  padding: const EdgeInsets.all(12),
                  decoration: BoxDecoration(
                    color: Colors.white.withValues(alpha: 0.04),
                    borderRadius: BorderRadius.circular(12),
                    border: Border.all(
                      color: Colors.white.withValues(alpha: 0.06),
                    ),
                  ),
                  child: Row(
                    children: [
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(
                              l.name.isEmpty ? '(unnamed)' : l.name,
                              style: const TextStyle(
                                color: BrandColors.lampWhite,
                                fontWeight: FontWeight.w600,
                              ),
                            ),
                            Text(
                              '${l.id} · ${l.rssi} dBm',
                              style: const TextStyle(
                                color: BrandColors.slateGrey,
                                fontSize: 11,
                                fontFamily: 'monospace',
                              ),
                            ),
                          ],
                        ),
                      ),
                      Container(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 8,
                          vertical: 4,
                        ),
                        decoration: BoxDecoration(
                          borderRadius: BorderRadius.circular(999),
                          color: (l.isConfigured
                                  ? BrandColors.lumenGreen
                                  : BrandColors.amberGold)
                              .withValues(alpha: 0.18),
                        ),
                        child: Text(
                          l.isConfigured ? 'configured' : 'unconfigured',
                          style: TextStyle(
                            fontSize: 10,
                            color: l.isConfigured
                                ? BrandColors.lumenGreen
                                : BrandColors.amberGold,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                      ),
                    ],
                  ),
                );
              },
            ),
    );
  }
}
```

- [ ] **Step 2: Add route**

In `software/lamp-app-flutter/lib/core/routing/router.dart`, add an import:

```dart
import '../../features/nearby/presentation/nearby_lamps_screen.dart';
```

And add a route entry inside `routes: [...]`:

```dart
GoRoute(
  path: '/devices',
  builder: (_, _) => const NearbyLampsScreen(),
),
```

- [ ] **Step 3: Surface a debug nav from the onboarding placeholder**

In `software/lamp-app-flutter/lib/features/onboarding/presentation/onboarding_placeholder.dart`, replace the body to add a `Devices` button. Add the import:

```dart
import 'package:go_router/go_router.dart';
```

And replace the `Column` children list with:

```dart
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
  '+ Add a lamp (coming in Phase 1b)',
  style: TextStyle(color: BrandColors.fogGrey),
),
const SizedBox(height: 24),
TextButton(
  onPressed: () => context.go('/devices'),
  child: const Text('Live scan (debug)'),
),
```

The whole widget will still be const where it can; the `TextButton` with a closure prevents the *outer* `Column` from being const, so drop `const` from the `Column` constructor and use `const` on each child instead.

- [ ] **Step 4: Codegen + analyze + test**

```bash
dart run build_runner build --delete-conflicting-outputs
flutter analyze
flutter test
```

- [ ] **Step 5: Commit**

```bash
git add software/lamp-app-flutter/lib/features/nearby/presentation \
        software/lamp-app-flutter/lib/core/routing/router.dart \
        software/lamp-app-flutter/lib/core/routing/router.g.dart \
        software/lamp-app-flutter/lib/features/onboarding/presentation/onboarding_placeholder.dart
git commit -m "$(cat <<'EOF'
feat(nearby): dev-only /devices screen + debug entry from onboarding

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: On-device manual verification

**Files:** none — this task is an instrumented run on the user's Pixel 10. No test code, no commit.

- [ ] **Step 1: Install + launch**

From the worktree root:
```bash
npm run app:install
```

- [ ] **Step 2: Walk through these checks on the phone**

| Check | Pass criterion |
|---|---|
| Cold launch (first install) | Bluetooth permission prompt appears. Tap Allow. |
| After granting | Onboarding screen with "+ Add a lamp" and "Live scan (debug)" button is visible. |
| Tap "Live scan (debug)" | `/devices` route opens. "Scanning..." shown initially. |
| Power on a configured lamp nearby | Within ~5s, the lamp appears with the **configured** pill, the right id, and a reasonable rssi (-80 to -30). |
| Power on an unconfigured (factory) lamp nearby | Same but with the **unconfigured** pill. |
| Power off the lamp + wait 30s | The row disappears (stale eviction). |
| Hot restart the app on the phone | Permissions are remembered (no prompt), `/devices` works on cold launch via `flutter run -d <id>` + push `g/devices`. |
| Deny permission | Friendly "Bluetooth permission needed" screen with Retry button. Tap Retry — system prompt re-appears (or settings link if "don't ask again" was selected). |

- [ ] **Step 3: Document findings**

If anything in the table above fails, file the specific symptom (screenshot if visual) and the device's logcat output. The implementer should report DONE_WITH_CONCERNS in that case and we'll patch in a follow-up commit before starting Phase 1b.

If everything passes, report DONE.

---

## Done — Phase 1a complete

Acceptance:
- `flutter analyze` clean.
- `flutter test` reports ≥34 passing (Phase 0's 30 + new tests in Tasks 3, 4, 5, 6, 7).
- The Devices screen surfaces real lamps over the air.

Branch stays local per the standing instruction. **Do not push.**

Next plan: **Phase 1b — Onboarding 4-step wizard (Scan → Name → WiFi → Done) + adopt flow**.
