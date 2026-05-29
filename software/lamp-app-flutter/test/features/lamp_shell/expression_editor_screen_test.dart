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
import 'package:lamp_app/features/lamp_shell/presentation/expression_editor_screen.dart';

const _devId = 'lamp-x';

/// Build a container with BLE seeded and inventory populated.
/// The controlNotifier is NOT pre-primed — the widget's watch primes it.
/// Pre-priming closes a temporary subscription inside FakeAsync, which
/// schedules Riverpod's 0-ms dispose timer and fails with "pending timer".
Future<ProviderContainer> _withEmptyState() async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
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
      Uint8List.fromList(utf8.encode('[]')));
  await ble.disconnect(_devId);

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
  testWidgets('new-expression editor shows Type / Target / Test / Save',
      (tester) async {
    final c = await _withEmptyState();
    addTearDown(c.dispose);
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: ExpressionEditorScreen(
          lampId: _devId,
          typeKey: '_new',
          targetKey: 3,
        ),
      ),
    ));
    await _pumpToData(tester, 'Type');
    expect(find.text('Type'), findsOneWidget);
    expect(find.text('Shade'), findsOneWidget);
    // Test and Save buttons may be below the fold — scroll to reveal them.
    await tester.dragUntilVisible(
      find.text('Test'),
      find.byType(ListView),
      const Offset(0, -200),
    );
    expect(find.text('Test'), findsOneWidget);
    expect(find.text('Save'), findsOneWidget);
  });
}
