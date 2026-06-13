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
import 'package:lamp_app/core/widgets/info_panel.dart';
import 'package:lamp_app/features/control/presentation/control_screen.dart';
import 'package:lamp_app/features/control/presentation/widgets/base_card.dart';
import 'package:lamp_app/features/control/presentation/widgets/brightness_card.dart';
import 'package:lamp_app/features/control/presentation/widgets/bt_only_info_pane.dart';
import 'package:lamp_app/features/control/presentation/widgets/connecting_view.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/nearby/application/nearby_lamps_notifier.dart';
import 'package:lamp_app/features/nearby/domain/nearby_lamp.dart';

import '../../../_support/seed.dart';

const _devId = 'lamp-x';

Future<ProviderContainer> _withLamp() async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await seedControlBle(ble, deviceId: _devId, name: 'jacko');
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

/// Inventory + BLE state where the firmware's auth gate would reject our
/// stored credential: lampSection reads back empty bytes. controlNotifier's
/// canary trips and the provider goes to AsyncError(LampAuthRequiredException).
Future<(ProviderContainer, InMemoryBleClient)> _withAuthGatedLamp() async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await seedControlBle(ble, deviceId: _devId, name: 'jacko');
  // Wipe lampSection so the canary read returns empty bytes.
  await ble.connect(_devId);
  await ble.write(_devId, BleUuids.controlService, BleUuids.lampSection,
      Uint8List(0));
  await ble.disconnect(_devId);
  final c = ProviderContainer(
    overrides: [
      bleClientProvider.overrideWithValue(ble),
      bleScannerProvider.overrideWithValue(FakeBleScanner()),
    ],
  );
  await c.read(inventoryNotifierProvider.future);
  // Empty password mirrors the "stored credential no longer satisfies the
  // gate" scenario this flow is built for.
  await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
        id: _devId, name: 'jacko', controlPassword: ''));
  return (c, ble);
}

Future<void> _pumpUntil(WidgetTester tester, Finder finder,
    {int frames = 60}) async {
  for (var i = 0; i < frames; i++) {
    await tester.pump(const Duration(milliseconds: 16));
    if (finder.evaluate().isNotEmpty) return;
  }
}

Future<void> _pumpToData(WidgetTester tester) async {
  for (var i = 0; i < 30; i++) {
    await tester.pump(const Duration(milliseconds: 16));
    if (find.byType(BrightnessCard).evaluate().isNotEmpty) return;
  }
}

void main() {
  testWidgets('BrightnessCard sits below BaseCard', (tester) async {
    final c = await _withLamp();
    addTearDown(c.dispose);
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: ControlScreen(lampId: _devId)),
      ),
    ));
    await _pumpToData(tester);
    await tester.dragUntilVisible(
      find.byType(BrightnessCard),
      find.byType(ListView),
      const Offset(0, -200),
    );
    final brightnessY = tester.getTopLeft(find.byType(BrightnessCard)).dy;
    final baseY = tester.getTopLeft(find.byType(BaseCard)).dy;
    expect(brightnessY, greaterThan(baseY),
        reason: 'Brightness must be visually below the base card');
  });

  testWidgets('BtOnlyInfoPane renders when active lamp is off-mesh',
      (tester) async {
    final c = await _withLamp();
    addTearDown(c.dispose);
    c.read(nearbyLampsNotifierProvider.notifier).state = [
      const NearbyLamp(
        id: _devId,
        name: 'jacko',
        rssi: -50,
        serviceUuids: [],
        baseRgb: 0,
        shadeRgb: 0,
        lastSeenEpochMs: 0,
        isMesh: false,
      ),
    ];
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: ControlScreen(lampId: _devId)),
      ),
    ));
    await _pumpToData(tester);
    expect(find.byType(BtOnlyInfoPane), findsOneWidget);
    expect(find.byType(InfoPanel), findsOneWidget);
  });

  testWidgets('BtOnlyInfoPane hides when active lamp is on-mesh',
      (tester) async {
    final c = await _withLamp();
    addTearDown(c.dispose);
    c.read(nearbyLampsNotifierProvider.notifier).state = [
      const NearbyLamp(
        id: _devId,
        name: 'jacko',
        rssi: -50,
        serviceUuids: [],
        baseRgb: 0,
        shadeRgb: 0,
        lastSeenEpochMs: 0,
        isMesh: true,
      ),
    ];
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: ControlScreen(lampId: _devId)),
      ),
    ));
    await _pumpToData(tester);
    // BtOnlyInfoPane is still mounted but renders SizedBox.shrink — the
    // assertion is on InfoPanel absence, since that's what actually paints.
    expect(find.byType(InfoPanel), findsNothing);
  });

  // ---------------------------------------------------------------------------
  // Connect-time password prompt: when the stored credential no longer
  // satisfies the firmware's auth gate, ControlScreen surfaces a dialog over
  // ConnectingView instead of dying with a generic FormatException.
  // ---------------------------------------------------------------------------

  testWidgets(
      'shows password dialog over ConnectingView when auth is required',
      (tester) async {
    final (c, _) = await _withAuthGatedLamp();
    addTearDown(c.dispose);
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: ControlScreen(lampId: _devId)),
      ),
    ));
    await _pumpUntil(tester, find.text('Enter password'));
    expect(find.text('Enter password'), findsOneWidget);
    expect(find.byType(ConnectingView), findsOneWidget,
        reason: 'ConnectingView stays behind the dialog');
  });

  testWidgets(
      'Connect with correct password closes dialog and renders control surface',
      (tester) async {
    final (c, ble) = await _withAuthGatedLamp();
    addTearDown(c.dispose);
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: ControlScreen(lampId: _devId)),
      ),
    ));
    await _pumpUntil(tester, find.text('Enter password'));

    // Simulate firmware "unlock": now reads of lampSection return real bytes.
    await ble.connect(_devId);
    await ble.write(
        _devId,
        BleUuids.controlService,
        BleUuids.lampSection,
        Uint8List.fromList(utf8.encode(
          '{"name":"jacko","brightness":50,"advancedEnabled":false}',
        )));

    await tester.enterText(find.byType(TextField), 'alpha');
    await tester.tap(find.widgetWithText(FilledButton, 'Connect'));
    await _pumpUntil(tester, find.byType(BrightnessCard));

    expect(find.text('Enter password'), findsNothing);
    expect(find.byType(BrightnessCard), findsOneWidget);
  });

  testWidgets(
      'Connect with wrong password shows inline error and keeps dialog open',
      (tester) async {
    final (c, _) = await _withAuthGatedLamp();
    addTearDown(c.dispose);
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: ControlScreen(lampId: _devId)),
      ),
    ));
    await _pumpUntil(tester, find.text('Enter password'));

    // Do NOT unlock: lampSection stays empty, so the canary in
    // submitConnectPassword fails again.
    await tester.enterText(find.byType(TextField), 'nope');
    await tester.tap(find.widgetWithText(FilledButton, 'Connect'));
    await _pumpUntil(tester, find.textContaining('Wrong password'));

    expect(find.text('Enter password'), findsOneWidget,
        reason: 'Dialog stays open so the user can retry');
    expect(find.textContaining('Wrong password'), findsOneWidget);
  });
}
