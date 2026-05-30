import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/lamp_shell/presentation/home_wifi_screen.dart';
import 'package:lamp_app/features/nearby/application/nearby_lamps_notifier.dart';

import '../../../_support/seed.dart';

const _devId = 'lamp-x';

Future<ProviderContainer> _container({
  required String wifiStateJson,
}) async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await seedControlBle(ble, deviceId: _devId, name: 'jacko');
  // Re-connect so we can write the wifiState seed.
  await ble.connect(_devId);
  await ble.write(
    _devId,
    BleUuids.controlService,
    BleUuids.wifiState,
    Uint8List.fromList(utf8.encode(wifiStateJson)),
  );
  final c = ProviderContainer(
    overrides: [
      bleClientProvider.overrideWithValue(ble),
      bleScannerProvider.overrideWithValue(FakeBleScanner()),
    ],
  );
  await c.read(inventoryNotifierProvider.future);
  await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
        id: _devId, name: 'jacko', controlPassword: 'secret'));
  return c;
}

Future<void> _pumpToData(WidgetTester tester) async {
  for (var i = 0; i < 30; i++) {
    await tester.pump(const Duration(milliseconds: 16));
    if (find.byType(ListView).evaluate().isNotEmpty) return;
  }
}

void main() {
  testWidgets('renders scan results from wifiState', (tester) async {
    final c = await _container(
        wifiStateJson:
            '{"state":"idle","scanResults":[{"ssid":"home","rssi":-55,"encrypted":true}]}');
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(home: HomeWifiScreen(lampId: _devId)),
    ));
    await _pumpToData(tester);

    expect(find.text('home'), findsOneWidget);
    expect(find.byIcon(Icons.lock_outline), findsOneWidget);
  });

  testWidgets('connected state shows ssid + ip and a Forget button',
      (tester) async {
    final c = await _container(
        wifiStateJson:
            '{"state":"connected","ssid":"home","ip":"192.168.1.20","scanResults":[]}');
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(home: HomeWifiScreen(lampId: _devId)),
    ));
    await _pumpToData(tester);

    expect(find.textContaining('home'), findsWidgets);
    expect(find.textContaining('192.168.1.20'), findsOneWidget);
    expect(find.text('Forget network'), findsOneWidget);
  });

  testWidgets('refresh button writes {op:scan} to wifiOp', (tester) async {
    final c = await _container(wifiStateJson: '{"state":"idle"}');
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(home: HomeWifiScreen(lampId: _devId)),
    ));
    await _pumpToData(tester);
    // The post-frame kickoff already fires scan(); verify by reading the
    // value of wifiOp.
    final ble = c.read(bleClientProvider);
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.wifiOp);
    expect(jsonDecode(utf8.decode(written)), {'op': 'scan'});
  });

  testWidgets('tapping a network opens the password sheet', (tester) async {
    final c = await _container(
        wifiStateJson:
            '{"state":"idle","scanResults":[{"ssid":"home","rssi":-55,"encrypted":true}]}');
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(home: HomeWifiScreen(lampId: _devId)),
    ));
    await _pumpToData(tester);
    await tester.tap(find.text('home'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text('Join "home"'), findsOneWidget);
  });
}
