import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/inventory/application/active_lamp_notifier.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/onboarding/application/add_lamp_notifier.dart';
import 'package:lamp_app/features/onboarding/domain/add_lamp_state.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  setUp(() {
    SharedPreferences.setMockInitialValues({});
    AddLampNotifier.verifyDelay = Duration.zero;
  });
  tearDown(() {
    AddLampNotifier.verifyDelay = const Duration(seconds: 5);
  });

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

  test('setName and setPassword update fields', () {
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);
    final n = c.read(addLampNotifierProvider.notifier);
    n.setName('jacko');
    n.setPassword('secret');
    final s = c.read(addLampNotifierProvider);
    expect(s.name, 'jacko');
    expect(s.password, 'secret');
  });

  test('submit() with valid post-claim lampSection persists + sets active',
      () async {
    final ble = InMemoryBleClient();
    // Seed the lampSection so the post-claim probe finds a valid JSON.
    await ble.connect('dev1');
    await ble.write(
      'dev1',
      BleUuids.controlService,
      BleUuids.lampSection,
      Uint8List.fromList(utf8.encode('{"name":"jacko","brightness":50}')),
    );
    await ble.disconnect('dev1');

    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);
    await c.read(inventoryNotifierProvider.future);
    await c.read(activeLampNotifierProvider.future);

    final n = c.read(addLampNotifierProvider.notifier);
    await n.select('dev1');
    n.setName('jacko');
    n.setPassword('secret');
    await n.submit();

    final s = c.read(addLampNotifierProvider);
    expect(s.step, AddLampStep.done);
    expect(s.status, AddLampStatus.idle);
    expect(s.error, AddLampError.none);

    final inv = await c.read(inventoryNotifierProvider.future);
    expect(inv.map((l) => l.id).toList(), ['dev1']);
    expect(inv.first.controlPassword, 'secret');

    final active = await c.read(activeLampNotifierProvider.future);
    expect(active, 'dev1');
  });

  test('submit() bounces back to password step on wrong password', () async {
    // Don't seed lampSection — the post-claim read will throw BleNotFound,
    // which the notifier treats as auth-rejected.
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);
    await c.read(inventoryNotifierProvider.future);
    await c.read(activeLampNotifierProvider.future);

    final n = c.read(addLampNotifierProvider.notifier);
    await n.select('dev1');
    n.setName('jacko');
    n.setPassword('wrong');
    await n.submit();

    final s = c.read(addLampNotifierProvider);
    expect(s.step, AddLampStep.password);
    expect(s.status, AddLampStatus.error);
    expect(s.error, AddLampError.wrongPassword);

    final inv = await c.read(inventoryNotifierProvider.future);
    expect(inv, isEmpty,
        reason: 'wrong password must not persist the lamp');
  });

  test('add(deviceId, name) skips wizard and adds to inventory', () async {
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);
    await c.read(inventoryNotifierProvider.future);
    await c.read(activeLampNotifierProvider.future);

    await c
        .read(addLampNotifierProvider.notifier)
        .add(deviceId: 'dev2', name: 'melonie');

    final inv = await c.read(inventoryNotifierProvider.future);
    expect(inv.map((l) => l.name).toList(), ['melonie']);

    final active = await c.read(activeLampNotifierProvider.future);
    expect(active, 'dev2');
  });
}
