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
import 'package:lamp_app/features/lamp_shell/presentation/setup_screen.dart';

const _devId = 'lamp-x';

Future<void> _seed(
  InMemoryBleClient ble, {
  String homeSsid = '',
  bool advancedEnabled = false,
}) async {
  await ble.connect(_devId);
  await ble.write(_devId, BleUuids.controlService, BleUuids.lampSection,
      Uint8List.fromList(utf8.encode(
        '{"name":"jacko","brightness":50,'
        '"advancedEnabled":$advancedEnabled}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.baseSection,
      Uint8List.fromList(utf8.encode(
        '{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"],"knockout":[]}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.shadeSection,
      Uint8List.fromList(utf8.encode(
        '{"px":38,"bpp":4,"colors":["#000000FF"]}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.homeSection,
      Uint8List.fromList(utf8.encode(
        '{"ssid":"$homeSsid","brightness":60}',
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

/// Build a container with BLE seeded and inventory populated.
/// The controlNotifier is NOT pre-primed here — pre-priming via
/// ProviderContainer.read() then closing the temporary subscription schedules
/// Riverpod's 0-ms dispose timer inside testWidgets' FakeAsync zone, which
/// causes "pending timer" failures. Instead the widget's watch primes it.
Future<ProviderContainer> _makeContainer({
  String homeSsid = '',
  bool advancedEnabled = false,
}) async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await _seed(ble, homeSsid: homeSsid, advancedEnabled: advancedEnabled);
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

/// Pump enough frames for InMemoryBleClient's async-but-synchronous operations
/// to complete so the provider reaches data state. ConnectingView runs an
/// infinite animation so pumpAndSettle would never converge.
Future<void> _pumpToData(WidgetTester tester) async {
  // Each BLE operation is an async function that resolves in one microtask.
  // Five sections × one hop each = 5 microtask bounces; a handful of pumps
  // drains the queue. We stop early once the Setup fields are visible.
  for (var i = 0; i < 30; i++) {
    await tester.pump(const Duration(milliseconds: 16));
    if (find.text('Lamp name').evaluate().isNotEmpty) return;
  }
}

void main() {
  testWidgets('Renders the lamp name in the name field', (tester) async {
    final c = await _makeContainer();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: SetupScreen(lampId: _devId)),
      ),
    ));
    await _pumpToData(tester);

    final field = find.widgetWithText(TextField, 'jacko');
    expect(field, findsOneWidget);
  });

  testWidgets('Hides MQTT section when home SSID is empty', (tester) async {
    final c = await _makeContainer(); // homeSsid: '' (default)
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: SetupScreen(lampId: _devId)),
      ),
    ));
    await _pumpToData(tester);

    expect(find.text('Smart Home (MQTT)'), findsNothing);
  });

  testWidgets('Shows MQTT section once home SSID is set', (tester) async {
    final c = await _makeContainer(homeSsid: 'home');
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: SetupScreen(lampId: _devId)),
      ),
    ));
    await _pumpToData(tester);

    expect(find.text('Smart Home (MQTT)'), findsOneWidget);
  });

  testWidgets('Hides Advanced section by default; reveals it when enabled',
      (tester) async {
    final c = await _makeContainer();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: SetupScreen(lampId: _devId)),
      ),
    ));
    await _pumpToData(tester);

    expect(find.text('Advanced'), findsNothing);
    expect(find.text('Enable advanced settings'), findsOneWidget);

    await tester.tap(find.text('Enable advanced settings'));
    await tester.pump();

    expect(find.text('Advanced'), findsOneWidget);
    expect(find.text('Enable advanced settings'), findsNothing);
  });
}
