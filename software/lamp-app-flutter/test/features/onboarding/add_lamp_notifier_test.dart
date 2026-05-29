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
