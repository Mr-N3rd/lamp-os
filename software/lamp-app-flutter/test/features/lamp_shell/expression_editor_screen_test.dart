import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/lamp_shell/presentation/expression_editor_screen.dart';

import 'dart:convert';

import '../../_support/seed.dart';

const _devId = 'lamp-x';

/// Build a container with BLE seeded and inventory populated.
/// The controlNotifier is NOT pre-primed — the widget's watch primes it.
/// Pre-priming closes a temporary subscription inside FakeAsync, which
/// schedules Riverpod's 0-ms dispose timer and fails with "pending timer".
Future<ProviderContainer> _withEmptyState() async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await seedControlBle(ble, deviceId: _devId, name: 'test');
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
  testWidgets('new-expression editor shows the picked type + target header',
      (tester) async {
    final c = await _withEmptyState();
    addTearDown(c.dispose);
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: ExpressionEditorScreen(
          lampId: _devId,
          typeKey: 'breathing',
          targetKey: 3,
        ),
      ),
    ));
    await _pumpToData(tester, 'Breathing');
    // Header carries the resolved type name + target.
    expect(find.text('Breathing'), findsOneWidget);
    expect(find.text('Target: Both'), findsOneWidget);

    // Test and Save buttons may be below the fold — scroll to reveal them.
    await tester.dragUntilVisible(
      find.text('Test'),
      find.byType(ListView),
      const Offset(0, -200),
    );
    expect(find.text('Test'), findsOneWidget);
    expect(find.text('Save'), findsOneWidget);
  });

  testWidgets('tapping Test writes to CHAR_EXPRESSION_TEST', (tester) async {
    final ble = InMemoryBleClient();
    SharedPreferences.setMockInitialValues({});
    await seedControlBle(ble, deviceId: _devId, name: 'test');
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

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: ExpressionEditorScreen(
          lampId: _devId,
          typeKey: 'breathing',
          targetKey: 3,
        ),
      ),
    ));
    await _pumpToData(tester, 'Breathing');
    await tester.dragUntilVisible(
      find.text('Test'),
      find.byType(ListView),
      const Offset(0, -200),
    );
    await tester.tap(find.text('Test'));
    await tester.pump();

    // The notifier's testExpression wrote to CHAR_EXPRESSION_TEST with the
    // firmware-expected envelope (a + type + target).
    await c.read(controlNotifierProvider(_devId).future);
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.expressionTest);
    final parsed = jsonDecode(utf8.decode(written)) as Map<String, dynamic>;
    expect(parsed['a'], 'test_expression');
    expect(parsed['type'], 'breathing');
    expect(parsed['target'], 3);
  });

  testWidgets('Save adds the entry to ControlState.expressions',
      (tester) async {
    final ble = InMemoryBleClient();
    SharedPreferences.setMockInitialValues({});
    await seedControlBle(ble, deviceId: _devId, name: 'test');
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

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: ExpressionEditorScreen(
          lampId: _devId,
          typeKey: 'breathing',
          targetKey: 3,
        ),
      ),
    ));
    await _pumpToData(tester, 'Breathing');
    await tester.dragUntilVisible(
      find.text('Save'),
      find.byType(ListView),
      const Offset(0, -200),
    );
    await tester.tap(find.text('Save'));
    // Drain microtasks for the async upsert.
    for (var i = 0; i < 30; i++) {
      await tester.pump(const Duration(milliseconds: 16));
      final list = c
          .read(controlNotifierProvider(_devId))
          .value!
          .expressions
          .expressions;
      if (list.isNotEmpty) break;
    }
    expect(
      c
          .read(controlNotifierProvider(_devId))
          .value!
          .expressions
          .expressions,
      hasLength(1),
    );
  });
}
