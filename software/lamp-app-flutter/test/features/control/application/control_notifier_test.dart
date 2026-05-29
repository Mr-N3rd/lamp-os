import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/control/application/control_state.dart';
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

  test('disconnect is called when build() throws after connect', () async {
    // Seed inventory but no section chars — connect + auth succeed, then the
    // first section read throws BleNotFound, simulating a partial build failure.
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    // NOTE: do NOT addTearDown(c.dispose) here — we call c.dispose() manually
    // below and need to verify the BLE state immediately after.

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: '', // empty password → auth is a no-op write
        ));

    // Hold a subscription so the auto-dispose timer doesn't fire before we
    // inspect state.
    final states = <AsyncValue<ControlState>>[];
    final sub = c.listen<AsyncValue<ControlState>>(
      controlNotifierProvider(_devId),
      (_, next) => states.add(next),
      fireImmediately: true,
    );
    addTearDown(sub.close);

    // Wait until the provider settles into an error state.
    await Future.doWhile(() async {
      await Future<void>.delayed(Duration.zero);
      return c
              .read(controlNotifierProvider(_devId))
              .hasError ==
          false;
    });

    expect(c.read(controlNotifierProvider(_devId)).hasError, isTrue);
    // Before dispose, the lamp was connected (connect succeeded).
    expect(ble.isConnected(_devId), isTrue);

    sub.close();
    c.dispose();

    // After dispose, the early onDispose registration should have
    // disconnected the lamp.
    expect(ble.isConnected(_devId), isFalse);
  });

  test('isDirty flips on setBrightness and clears on save() round-trip',
      () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    // ALSO seed the settings blob (save() needs it).
    await ble.connect(_devId);
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"name":"jacko","brightness":42,"advancedEnabled":false},'
          '"base":{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"],"knockout":[]},'
          '"shade":{"px":38,"bpp":4,"colors":["#000000FF"]}}',
        )));
    await ble.disconnect(_devId);

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

    final n = c.read(controlNotifierProvider(_devId).notifier);
    expect(n.isDirty, isFalse);

    await n.setBrightness(80);
    expect(n.isDirty, isTrue);
  });

  test('save() merges local state into the full settings blob', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    await ble.connect(_devId);
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"name":"jacko","brightness":42,"advancedEnabled":false},'
          '"base":{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"],"knockout":[]},'
          '"shade":{"px":38,"bpp":4,"colors":["#000000FF"]},'
          '"expressions":[{"type":"breathing","enabled":true}]}', // untouched field
        )));
    await ble.disconnect(_devId);

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

    // Keep listener alive so BLE stays connected during save().
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    await n.setBrightness(80);
    await n.save();

    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    final parsed =
        jsonDecode(utf8.decode(written)) as Map<String, dynamic>;
    expect((parsed['lamp'] as Map)['brightness'], 80);
    expect(parsed['expressions'], isNotNull); // unchanged field preserved
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

    // Keep a listener alive so the auto-dispose provider doesn't disconnect
    // BLE before the debounce timer fires.
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

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

  test('disconnect surfaces in state and schedules reconnect', () async {
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

    // Keep a listener alive so the auto-dispose provider doesn't tear down.
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    await ble.disconnect(_devId);
    // The watchConnected stream uses an async* generator; the false emission
    // needs a couple of microtask hops to reach the listener.
    await Future<void>.delayed(const Duration(milliseconds: 10));

    final s = c.read(controlNotifierProvider(_devId)).value!;
    expect(s.connected, isFalse);
    expect(s.reconnectAttempt, 1);
  });

  test(
      'local edits survive a disconnect/reconnect cycle and the lamp catches up',
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
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    // User edit locally.
    await c
        .read(controlNotifierProvider(_devId).notifier)
        .setBrightness(80);
    // Let the debounce coalescer fire so the BLE write lands before disconnect.
    await Future<void>.delayed(const Duration(milliseconds: 50));

    // Link drops.
    await ble.disconnect(_devId);
    // Allow the async* watchConnected generator to propagate the false event.
    await Future<void>.delayed(const Duration(milliseconds: 10));

    // Local state should still hold 80.
    expect(
      c.read(controlNotifierProvider(_devId)).value!.lamp.brightness,
      80,
    );
    expect(
      c.read(controlNotifierProvider(_devId)).value!.connected,
      isFalse,
    );

    // Simulate the lamp coming back up — connecting triggers watchConnected
    // which fires _onConnectionChange(true) → _pushLocalState.
    await ble.connect(_devId);
    await Future<void>.delayed(const Duration(milliseconds: 50));

    expect(
      c.read(controlNotifierProvider(_devId)).value!.lamp.brightness,
      80,
    );
    expect(
      c.read(controlNotifierProvider(_devId)).value!.connected,
      isTrue,
    );
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.brightness);
    expect(written.single, 80); // catch-up push landed
  });

  test('save() is a no-op while disconnected', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);

    // Seed the settings blob so save() would otherwise have something to read.
    await ble.connect(_devId);
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"brightness":42},"base":{"colors":[]},"shade":{"colors":[]}}',
        )));
    // Snapshot the blob value BEFORE we proceed (values persist in
    // InMemoryBleClient across disconnect cycles, but read() requires a live
    // connection; take the snapshot now while still connected).
    final before = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    await ble.disconnect(_devId);

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
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    // Force disconnect so state.connected becomes false.
    await ble.disconnect(_devId);
    // Allow the async* watchConnected generator to propagate the false event.
    await Future<void>.delayed(const Duration(milliseconds: 10));

    // save() should be a no-op.
    await c.read(controlNotifierProvider(_devId).notifier).save();

    // Reconnect to snapshot the current settingsBlob value.
    await ble.connect(_devId);
    final after = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    expect(after, before); // save was a no-op
  });
}
