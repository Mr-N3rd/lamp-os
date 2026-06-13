import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/lamp_shell/presentation/info_screen.dart';
import 'package:lamp_app/features/nearby/application/nearby_lamps_notifier.dart';

import '../../../_support/seed.dart';

const _devId = 'lamp-x';

Future<ProviderContainer> _container({
  int? fwVersion,
  String? fwChannel,
}) async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await seedControlBle(
    ble,
    deviceId: _devId,
    name: 'jacko',
    fwVersion: fwVersion,
    fwChannel: fwChannel,
  );
  final c = ProviderContainer(
    retry: (_, _) => null,
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

Widget _wrap(ProviderContainer c) => UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: InfoScreen(lampId: _devId)),
      ),
    );

/// Pump until the firmware version line shows up. The Info screen reads
/// the lamp section via controlNotifierProvider which is async; a fixed
/// number of frames is enough to drive the future to ready without
/// risking an infinite-animation pumpAndSettle hang.
Future<void> _pumpUntilFirmware(WidgetTester tester) async {
  for (var i = 0; i < 60; i++) {
    await tester.pump(const Duration(milliseconds: 16));
    if (find.textContaining('Firmware ').evaluate().isNotEmpty &&
        !find.text('Firmware ...').evaluate().isNotEmpty) {
      return;
    }
  }
}

void main() {
  setUpAll(() {
    // App-version line uses package_info_plus; the platform channel isn't
    // wired in widget tests, so stub the singleton instead.
    PackageInfo.setMockInitialValues(
      appName: 'lamp_app',
      packageName: 'ca.lamplit.lampapp',
      version: '1.0.0',
      buildNumber: '1',
      buildSignature: '',
    );
  });

  testWidgets('renders firmware + app version when lamp emits fwVersion',
      (tester) async {
    final c = await _container(fwVersion: 0x010000, fwChannel: 'stable');
    addTearDown(c.dispose);

    await tester.pumpWidget(_wrap(c));
    await _pumpUntilFirmware(tester);

    // Firmware line uses the lamp section's fwVersion/fwChannel.
    expect(find.text('Firmware 1.0.0 (stable)'), findsOneWidget);
    // App line uses the stubbed PackageInfo + the hardcoded kAppChannel.
    expect(find.text('App 1.0.0+1 (dev)'), findsOneWidget);
  });

  testWidgets('renders placeholder firmware text when lamp omits fwVersion',
      (tester) async {
    // Legacy firmware: no fwVersion/fwChannel fields in CHAR_LAMP_SECTION.
    // The Info tab must not crash; it shows a placeholder instead.
    final c = await _container();
    addTearDown(c.dispose);

    await tester.pumpWidget(_wrap(c));
    // Pump enough frames for controlNotifier to settle.
    for (var i = 0; i < 60; i++) {
      await tester.pump(const Duration(milliseconds: 16));
      if (find.textContaining('Firmware ').evaluate().isNotEmpty) break;
    }
    expect(find.text('Firmware ...'), findsOneWidget);
  });
}
