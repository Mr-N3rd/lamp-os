import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';
import 'package:lamp_app/core/widgets/lamp_chip.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/lamp_shell/presentation/expressions_screen.dart';
import 'package:lamp_app/features/lamp_shell/presentation/lamp_shell.dart';
import 'package:lamp_app/features/nearby/application/nearby_lamps_notifier.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../../_support/scan_grace_override.dart';
import '../../_support/seed.dart';

/// Build a container with both the BLE client and scanner stubbed out so
/// LampShell's transitive watch of nearbyLampsNotifierProvider doesn't
/// reach for the real flutter_blue_plus platform (unsupported in tests).
ProviderContainer _container() {
  return ProviderContainer(
    // Disable Riverpod's default exponential-backoff retry: this test
    // intentionally lets controlNotifier's build() error out (the BLE client
    // has no seeded characteristics), and the retry timer would otherwise
    // outlive the test widget and trip the pending-timer assertion.
    retry: (_, _) => null,
    overrides: [
      bleClientProvider.overrideWithValue(InMemoryBleClient()),
      bleScannerProvider.overrideWithValue(FakeBleScanner()),
      scanGraceTestOverride,
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
    final ble = InMemoryBleClient();
    await seedControlBle(ble, deviceId: 'lamp-1', name: 'jacko');
    final c = ProviderContainer(
      retry: (_, _) => null,
      overrides: [
        bleClientProvider.overrideWithValue(ble),
        bleScannerProvider.overrideWithValue(FakeBleScanner()),
        scanGraceTestOverride,
      ],
    );
    addTearDown(c.dispose);
    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: 'lamp-1', name: 'jacko', controlPassword: 'secret'));

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1', initialTab: LampTab.control),
      ),
    ));
    // Seeded BLE + inventory ⇒ ControlNotifier reaches connected=true,
    // which un-gates the bottom-nav tap handlers (IgnorePointer wraps
    // the NavigationBar when disconnected). Wait for connected to flip.
    for (var i = 0; i < 40; i++) {
      await tester.pump(const Duration(milliseconds: 16));
      final state = c.read(controlNotifierProvider('lamp-1')).value;
      if (state?.connected == true) break;
    }
    expect(find.byType(NavigationBar), findsOneWidget);

    // Tap by the nav icon to avoid hit-testing through stacked layers — the
    // 'Expressions' label is inside a NavigationDestination tile.
    await tester.tap(find.byIcon(Icons.auto_awesome));
    await _settle(tester);
    expect(find.byType(ExpressionsScreen), findsOneWidget);

    // Social tab — Wisp tab is gone from the bottom nav (moved behind
    // the 5-tap-orbs gesture on the WispIndicator), so Social is now
    // the third bottom-nav destination. Switching here verifies
    // tab-switch coverage to a still-present destination.
    await tester.tap(find.byIcon(Icons.handshake_outlined));
    await _settle(tester);
    expect(find.byType(ExpressionsScreen), findsNothing);
  });

  testWidgets('NavigationBar is ignore-pointered while disconnected',
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

    // Without seeded BLE / inventory the control notifier never reaches
    // connected==true, so the tab nav is wrapped in IgnorePointer +
    // Opacity. Tap attempts should be silently dropped and the tab must
    // stay on Control.
    await tester.tap(find.byIcon(Icons.auto_awesome), warnIfMissed: false);
    await _settle(tester);
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

  testWidgets('Save action appears on editing tabs, hidden on Wisp',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final ble = InMemoryBleClient();
    await seedControlBle(ble, deviceId: 'lamp-1', name: 'jacko');
    final c = ProviderContainer(
      retry: (_, _) => null,
      overrides: [
        bleClientProvider.overrideWithValue(ble),
        bleScannerProvider.overrideWithValue(FakeBleScanner()),
        scanGraceTestOverride,
      ],
    );
    addTearDown(c.dispose);
    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: 'lamp-1', name: 'jacko', controlPassword: 'secret'));

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1', initialTab: LampTab.control),
      ),
    ));
    // Wait for connected==true so the bottom nav is tappable. Tab
    // switching is gated on the connection now (IgnorePointer in the
    // shell), and the test needs to drive switches.
    for (var i = 0; i < 40; i++) {
      await tester.pump(const Duration(milliseconds: 16));
      final state = c.read(controlNotifierProvider('lamp-1')).value;
      if (state?.connected == true) break;
    }
    // Connected + clean ⇒ Save action present as outlined "Saved" chip on
    // Setup tab.
    expect(find.text('Saved'), findsOneWidget);

    // Switch to Expressions — Save action stays (expressions now ride
    // the unified isDirty + settings-blob save path).
    await tester.tap(find.byIcon(Icons.auto_awesome));
    await _settle(tester);
    expect(find.text('Saved'), findsOneWidget);

    // Switch to Info — read-only branding + firmware + version, no
    // dirty save flow; Save action hidden. (Wisp tab is gone — its
    // configuration screen has its own AppBar with no Save action.)
    await tester.tap(find.byIcon(Icons.info_outline));
    await _settle(tester);
    expect(find.text('Saved'), findsNothing);
    expect(find.text('Save changes'), findsNothing);
    expect(find.text('Reconnecting…'), findsNothing);
  });

  testWidgets('Save action shows "Save changes" pill when dirty',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final ble = InMemoryBleClient();
    await seedControlBle(ble, deviceId: 'lamp-1', name: 'jacko');
    final c = ProviderContainer(
      retry: (_, _) => null,
      overrides: [
        bleClientProvider.overrideWithValue(ble),
        bleScannerProvider.overrideWithValue(FakeBleScanner()),
        scanGraceTestOverride,
      ],
    );
    addTearDown(c.dispose);
    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: 'lamp-1', name: 'jacko', controlPassword: 'secret'));

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1', initialTab: LampTab.control),
      ),
    ));
    // Wait for controlNotifier to reach data state with connected==true.
    for (var i = 0; i < 40; i++) {
      await tester.pump(const Duration(milliseconds: 16));
      if (find.text('Saved').evaluate().isNotEmpty) break;
    }
    // Connected + clean ⇒ "Saved" outlined chip.
    expect(find.text('Saved'), findsOneWidget);

    // Make the state dirty by changing the lamp name through the notifier.
    await c
        .read(controlNotifierProvider('lamp-1').notifier)
        .setLampName('renamed');
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 16));

    // Dirty ⇒ gradient pill swaps to "Save changes". The red-dot badge
    // was removed: the entire pill lighting up already signals dirty.
    expect(find.text('Save changes'), findsOneWidget);
  });

  testWidgets('Save action shows outlined "Saved" when clean + connected',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    final ble = InMemoryBleClient();
    await seedControlBle(ble, deviceId: 'lamp-1', name: 'jacko');
    final c = ProviderContainer(
      retry: (_, _) => null,
      overrides: [
        bleClientProvider.overrideWithValue(ble),
        bleScannerProvider.overrideWithValue(FakeBleScanner()),
        scanGraceTestOverride,
      ],
    );
    addTearDown(c.dispose);
    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: 'lamp-1', name: 'jacko', controlPassword: 'secret'));

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: LampShell(lampId: 'lamp-1', initialTab: LampTab.control),
      ),
    ));
    for (var i = 0; i < 40; i++) {
      await tester.pump(const Duration(milliseconds: 16));
      if (find.text('Saved').evaluate().isNotEmpty) break;
    }
    expect(find.text('Saved'), findsOneWidget);
    expect(find.text('Save changes'), findsNothing);
  });
}
