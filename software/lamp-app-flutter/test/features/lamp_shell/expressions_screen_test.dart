import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/lamp_shell/presentation/expressions_screen.dart';

const _devId = 'lamp-x';

Future<void> _seed(InMemoryBleClient ble,
    {required String expressionsJson}) async {
  await ble.connect(_devId);
  await ble.write(_devId, BleUuids.controlService, BleUuids.lampSection,
      Uint8List.fromList(utf8.encode(
          '{"name":"test","brightness":50,"advancedEnabled":false}')));
  await ble.write(_devId, BleUuids.controlService, BleUuids.baseSection,
      Uint8List.fromList(utf8.encode(
          '{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"],"knockout":[]}')));
  await ble.write(_devId, BleUuids.controlService, BleUuids.shadeSection,
      Uint8List.fromList(
          utf8.encode('{"px":38,"bpp":4,"colors":["#000000FF"]}')));
  await ble.write(_devId, BleUuids.controlService, BleUuids.homeSection,
      Uint8List.fromList(utf8.encode('{"ssid":"","brightness":60}')));
  await ble.write(
      _devId,
      BleUuids.controlService,
      BleUuids.mqttSection,
      Uint8List.fromList(utf8.encode('{"enabled":false,"brokerHost":"",'
          '"brokerPort":1883,"username":"","topicPrefix":""}')));
  await ble.write(_devId, BleUuids.controlService, BleUuids.exprSection,
      Uint8List.fromList(utf8.encode(expressionsJson)));
  await ble.disconnect(_devId);
}

/// Build a container with BLE seeded and inventory populated.
/// The controlNotifier is NOT pre-primed — the widget's watch primes it.
/// Pre-priming closes a temporary subscription inside FakeAsync, which
/// schedules Riverpod's 0-ms dispose timer and fails with "pending timer".
Future<ProviderContainer> _withState(String expressionsJson) async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await _seed(ble, expressionsJson: expressionsJson);
  final c = ProviderContainer(
    overrides: [bleClientProvider.overrideWithValue(ble)],
  );
  await c.read(inventoryNotifierProvider.future);
  await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
        id: _devId,
        name: 'jacko',
        controlPassword: 'secret',
      ));
  return c;
}

/// Pump enough frames for InMemoryBleClient async operations to resolve.
/// ConnectingView animates infinitely so pumpAndSettle never converges.
Future<void> _pumpToData(WidgetTester tester, String sentinel) async {
  for (var i = 0; i < 30; i++) {
    await tester.pump(const Duration(milliseconds: 16));
    if (find.text(sentinel).evaluate().isNotEmpty) return;
  }
}

void main() {
  testWidgets('renders empty-state when no expressions', (tester) async {
    final c = await _withState('[]');
    addTearDown(c.dispose);
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: ExpressionsScreen(lampId: _devId),
      ),
    ));
    await _pumpToData(tester, 'No expressions yet');
    expect(find.text('No expressions yet'), findsOneWidget);
  });

  testWidgets('renders one tile per expression', (tester) async {
    final c = await _withState(
        '[{"type":"breathing","enabled":true,"colors":[],'
        '"intervalMin":10,"intervalMax":20,"target":1},'
        '{"type":"glitchy","enabled":false,"colors":[],'
        '"intervalMin":60,"intervalMax":900,"target":3}]');
    addTearDown(c.dispose);
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: ExpressionsScreen(lampId: _devId),
      ),
    ));
    await _pumpToData(tester, 'breathing');
    expect(find.text('breathing'), findsOneWidget);
    expect(find.text('glitchy'), findsOneWidget);
  });
}
