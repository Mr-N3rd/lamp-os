import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';
import 'package:lamp_app/core/widgets/info_panel.dart';
import 'package:lamp_app/features/control/presentation/control_screen.dart';
import 'package:lamp_app/features/control/presentation/widgets/base_card.dart';
import 'package:lamp_app/features/control/presentation/widgets/brightness_card.dart';
import 'package:lamp_app/features/control/presentation/widgets/bt_only_info_pane.dart';
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
        onMesh: false,
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
        onMesh: true,
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
}
