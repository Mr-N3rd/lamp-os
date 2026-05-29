import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/control/presentation/widgets/base_editor_sheet.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:shared_preferences/shared_preferences.dart';

// Test strategy: BaseEditorSheet is now a ConsumerWidget that reads live state
// from controlNotifierProvider. Tests create a ProviderContainer, seed
// inventory, then wrap with UncontrolledProviderScope — so the provider is
// ready before the widget tree starts watching it.

const _devId = 'lamp-x';

Future<void> _seedBle(InMemoryBleClient ble, {int stopCount = 2}) async {
  String hex(int v) =>
      v.toRadixString(16).padLeft(6, '0').toUpperCase();
  final colors = List.generate(
    stopCount,
    (i) => '"#${hex(((i + 1) * 0x300783) & 0xFFFFFF)}FF"',
  ).join(',');
  await ble.connect(_devId);
  await ble.write(_devId, BleUuids.controlService, BleUuids.lampSection,
      Uint8List.fromList(utf8.encode(
        '{"name":"test","brightness":50,"advancedEnabled":false}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.baseSection,
      Uint8List.fromList(utf8.encode(
        '{"px":35,"ac":0,"bpp":4,"colors":[$colors],"knockout":[]}',
      )));
  await ble.write(_devId, BleUuids.controlService, BleUuids.shadeSection,
      Uint8List.fromList(utf8.encode(
        '{"px":38,"bpp":4,"colors":["#000000FF"]}',
      )));
  await ble.disconnect(_devId);
}

/// Build a container, seed the inventory lamp, and await the control notifier
/// so state is ready before the widget tree renders.
Future<ProviderContainer> _buildContainer(
    InMemoryBleClient ble, {
    int stopCount = 2,
  }) async {
  await _seedBle(ble, stopCount: stopCount);
  final c = ProviderContainer(
    overrides: [bleClientProvider.overrideWithValue(ble)],
  );
  await c.read(inventoryNotifierProvider.future);
  await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
        id: _devId,
        name: 'test',
        controlPassword: '',
      ));
  // Prime the notifier so it's in data state when the widget first builds.
  await c.read(controlNotifierProvider(_devId).future);
  return c;
}

void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  testWidgets('renders SizedBox while notifier is in loading state',
      (tester) async {
    // No BLE data seeded and no inventory lamp — notifier stays loading.
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: BaseEditorSheet(lampId: _devId)),
      ),
    ));
    // Provider stays in loading — widget renders SizedBox.shrink().
    expect(find.text('+ Add stop'), findsNothing);
    expect(find.text('Base gradient'), findsNothing);
  });

  testWidgets('renders one row per stop and add-stop CTA when < 5 stops',
      (tester) async {
    final ble = InMemoryBleClient();
    final c = await _buildContainer(ble, stopCount: 2);
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: BaseEditorSheet(lampId: _devId)),
      ),
    ));
    await tester.pumpAndSettle();

    expect(find.text('Base gradient'), findsOneWidget);
    expect(find.text('+ Add stop'), findsOneWidget);
    expect(find.byIcon(Icons.drag_indicator), findsNWidgets(2));
  });

  testWidgets('hides add-stop CTA at 5 stops', (tester) async {
    final ble = InMemoryBleClient();
    final c = await _buildContainer(ble, stopCount: 5);
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: BaseEditorSheet(lampId: _devId)),
      ),
    ));
    await tester.pumpAndSettle();

    expect(find.text('+ Add stop'), findsNothing);
    expect(find.byIcon(Icons.drag_indicator), findsNWidgets(5));
  });
}
