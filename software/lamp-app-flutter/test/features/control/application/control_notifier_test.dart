import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
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
}
