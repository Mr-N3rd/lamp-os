import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/control/presentation/knockout_screen.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';

const _devId = 'lamp-x';

Future<void> _seed(InMemoryBleClient ble) async {
  await ble.connect(_devId);
  await ble.write(_devId, BleUuids.controlService, BleUuids.lampSection,
      Uint8List.fromList(utf8.encode(
        '{"name":"test","brightness":50,"advancedEnabled":false}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.baseSection,
      Uint8List.fromList(utf8.encode(
        '{"px":3,"ac":0,"bpp":4,"colors":["#300783FF"],"knockout":[{"p":1,"b":50}]}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.shadeSection,
      Uint8List.fromList(utf8.encode(
        '{"px":38,"bpp":4,"colors":["#000000FF"]}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.homeSection,
      Uint8List.fromList(utf8.encode(
        '{"ssid":"","brightness":60}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.mqttSection,
      Uint8List.fromList(utf8.encode(
        '{"enabled":false,"brokerHost":"","brokerPort":1883,'
        '"username":"","topicPrefix":""}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.exprSection,
      Uint8List.fromList(utf8.encode('[]')));
  await ble.disconnect(_devId);
}

Future<ProviderContainer> _withState() async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await _seed(ble);
  final c = ProviderContainer(
    overrides: [bleClientProvider.overrideWithValue(ble)],
  );
  await c.read(inventoryNotifierProvider.future);
  await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
        id: _devId,
        name: 'jacko',
        controlPassword: 'secret',
      ));
  await c.read(controlNotifierProvider(_devId).future);
  return c;
}

void main() {
  testWidgets('renders one row per LED with the right brightness label',
      (tester) async {
    final c = await _withState();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: KnockoutScreen(lampId: _devId),
      ),
    ));
    await tester.pumpAndSettle();

    // 3 rows for px = 3.
    expect(find.text('#0'), findsOneWidget);
    expect(find.text('#1'), findsOneWidget);
    expect(find.text('#2'), findsOneWidget);

    // Default rows show 100%, the seeded row shows 50%.
    expect(find.text('100%'), findsNWidgets(2));
    expect(find.text('50%'), findsOneWidget);
  });

  testWidgets('Reset all returns every pixel to 100%', (tester) async {
    final c = await _withState();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: KnockoutScreen(lampId: _devId),
      ),
    ));
    await tester.pumpAndSettle();

    await tester.tap(find.text('Reset all'));
    await tester.pumpAndSettle();

    expect(find.text('100%'), findsNWidgets(3));
    expect(find.text('50%'), findsNothing);
  });

  testWidgets('title shows the lamp name', (tester) async {
    final c = await _withState();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: KnockoutScreen(lampId: _devId),
      ),
    ));
    await tester.pumpAndSettle();
    expect(find.text('Pixel Knockout · jacko'), findsOneWidget);
  });
}
