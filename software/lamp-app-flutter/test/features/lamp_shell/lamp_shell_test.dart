import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';
import 'package:lamp_app/core/widgets/lamp_chip.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/lamp_shell/presentation/expressions_screen.dart';
import 'package:lamp_app/features/lamp_shell/presentation/lamp_shell.dart';
import 'package:lamp_app/features/lamp_shell/presentation/setup_screen.dart';
import 'package:lamp_app/features/nearby/application/nearby_lamps_notifier.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Build a container with both the BLE client and scanner stubbed out so
/// LampShell's transitive watch of nearbyLampsNotifierProvider doesn't
/// reach for the real flutter_blue_plus platform (unsupported in tests).
ProviderContainer _container() {
  return ProviderContainer(
    overrides: [
      bleClientProvider.overrideWithValue(InMemoryBleClient()),
      bleScannerProvider.overrideWithValue(FakeBleScanner()),
    ],
  );
}

/// pumpAndSettle would never converge because StatusDot inside LampChip
/// runs an indefinite repeat-reverse animation. Two staged pumps drain
/// the microtask + Navigator queues without depending on the animation
/// to ever settle.
Future<void> _settle(WidgetTester tester) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
}

void main() {
  testWidgets('renders Control by default, switches to Expressions on tap',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final c = _container();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1', initialTab: LampTab.control),
      ),
    ));
    await _settle(tester);
    // Without a seeded BLE/inventory the Control notifier may settle either
    // loading (no inventory entry → stuck on the future) or error (build
    // threw). Either way the LampShell scaffold + NavigationBar is rendered,
    // which is all we need to drive a tab switch below.
    expect(find.byType(NavigationBar), findsOneWidget);

    // Tap by the nav icon to avoid hit-testing through stacked layers — the
    // 'Expressions' label is inside a NavigationDestination tile.
    await tester.tap(find.byIcon(Icons.auto_awesome));
    await _settle(tester);
    expect(find.byType(ExpressionsScreen), findsOneWidget);

    await tester.tap(find.byIcon(Icons.settings));
    await _settle(tester);
    // SetupScreen is now the Setup tab; it renders either ConnectingView
    // (loading) or the error view depending on BLE state. Either way the
    // scaffold is present and the Expressions screen is not.
    expect(find.byType(SetupScreen), findsOneWidget);
    expect(find.byType(ExpressionsScreen), findsNothing);
  });

  testWidgets('AppBar shows a LampChip with the inventory name',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final c = _container();
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
    await _settle(tester);

    expect(find.byType(LampChip), findsOneWidget);
    expect(find.text('jacko'), findsOneWidget);
  });

  testWidgets('AppBar falls back to lampId when not in inventory',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final c = _container();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-99'),
      ),
    ));
    await _settle(tester);

    expect(find.text('lamp-99'), findsOneWidget);
  });

  testWidgets('Save action appears on Control tab, not on Expressions tab',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final c = _container();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1', initialTab: LampTab.control),
      ),
    ));
    await _settle(tester);

    // Save icon should be visible on the Control tab.
    expect(find.byIcon(Icons.save_outlined), findsOneWidget);

    // Switch to Expressions tab — Save icon should disappear.
    await tester.tap(find.byIcon(Icons.auto_awesome));
    await _settle(tester);
    expect(find.byIcon(Icons.save_outlined), findsNothing);
  });
}
