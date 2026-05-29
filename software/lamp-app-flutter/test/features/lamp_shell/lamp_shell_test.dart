import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/features/control/presentation/widgets/connecting_view.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/lamp_shell/presentation/lamp_shell.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  testWidgets('renders Control by default, switches to Expressions on tap',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1', initialTab: LampTab.control),
      ),
    ));
    // ControlScreen loads via Riverpod; without a seeded BLE/inventory the
    // notifier stays in loading state — ConnectingView is the expected output.
    expect(find.byType(ConnectingView), findsOneWidget);

    await tester.tap(find.text('Expressions'));
    await tester.pumpAndSettle();
    expect(find.text('Expressions · lamp-1'), findsOneWidget);

    await tester.tap(find.text('Setup'));
    await tester.pumpAndSettle();
    expect(find.text('Setup · lamp-1'), findsOneWidget);
  });

  testWidgets('shows lamp name from inventory in the AppBar', (tester) async {
    SharedPreferences.setMockInitialValues({});
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: 'lamp-1',
          name: 'jacko',
        ));

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1'),
      ),
    ));
    await tester.pumpAndSettle();

    expect(find.text('jacko'), findsOneWidget);
  });

  testWidgets('shows lampId as fallback title when not in inventory',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-99'),
      ),
    ));
    await tester.pump();

    expect(find.text('lamp-99'), findsOneWidget);
  });

  testWidgets('Save action appears on Control tab, not on Expressions tab',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1', initialTab: LampTab.control),
      ),
    ));
    await tester.pump();

    // Save icon should be visible on the Control tab.
    expect(find.byIcon(Icons.save_outlined), findsOneWidget);

    // Switch to Expressions tab — Save icon should disappear.
    await tester.tap(find.text('Expressions'));
    await tester.pumpAndSettle();
    expect(find.byIcon(Icons.save_outlined), findsNothing);
  });
}
