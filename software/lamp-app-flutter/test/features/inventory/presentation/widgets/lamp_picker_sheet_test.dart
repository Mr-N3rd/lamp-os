import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/core/ble/ble_scanner.dart';
import 'package:lamp_app/features/inventory/application/active_lamp_notifier.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/inventory/presentation/widgets/lamp_picker_sheet.dart';
import 'package:lamp_app/features/nearby/application/nearby_lamps_notifier.dart';
import 'package:lamp_app/features/nearby/domain/nearby_lamp.dart';

// Note: pumpAndSettle() times out because StatusDot runs an infinite
// AnimationController. Following the pattern from status_dot_test.dart
// and lamp_chip_test.dart we use pump(fixed duration) throughout.
//
// The tests that validate pop behaviour push LampPickerSheet onto a
// real Navigator route (not via showLampPickerSheet / FractionallySizedBox)
// so tiles stay within the 800×600 test viewport.
//
// settle() pumps two rounds of (microtask + 300 ms) because:
//  round 1 — flushes async providers (SharedPreferences.set, etc.)
//  round 2 — drains the resulting navigation/animation frames
// StatusDot has an infinite AnimationController so pumpAndSettle() would
// time out — fixed-duration pump() is the pattern used here.
Future<void> settle(WidgetTester tester) async {
  await tester.pump(); // microtask / async-notifier flush
  await tester.pump(const Duration(milliseconds: 300)); // anim frame 1
  await tester.pump(); // follow-up microtasks
  await tester.pump(const Duration(milliseconds: 300)); // anim frame 2
}

void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  // Build a ProviderContainer with a FakeBleScanner override (so
  // nearbyLampsNotifierProvider never touches real BLE on the test host)
  // and pre-populate inventory with [lamps].
  Future<ProviderContainer> withInventory(List<InventoryLamp> lamps) async {
    final c = ProviderContainer(
      overrides: [bleScannerProvider.overrideWithValue(FakeBleScanner())],
    );
    await c.read(inventoryNotifierProvider.future);
    for (final l in lamps) {
      await c.read(inventoryNotifierProvider.notifier).add(l);
    }
    return c;
  }

  testWidgets('renders one tile per inventory lamp with the lamp name',
      (tester) async {
    final c = await withInventory(const [
      InventoryLamp(id: 'a', name: 'jacko'),
      InventoryLamp(id: 'b', name: 'melonie'),
    ]);
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: LampPickerSheet(currentLampId: 'a')),
      ),
    ));
    await settle(tester);

    expect(find.text('jacko'), findsOneWidget);
    expect(find.text('melonie'), findsOneWidget);
    expect(find.text('active'), findsOneWidget); // only on the current row
  });

  testWidgets(
      'tapping a non-active inventory tile sets active + pops the sheet',
      (tester) async {
    final c = await withInventory(const [
      InventoryLamp(id: 'a', name: 'jacko'),
      InventoryLamp(id: 'b', name: 'melonie'),
    ]);
    await c.read(activeLampNotifierProvider.future);
    await c.read(activeLampNotifierProvider.notifier).set('a');
    addTearDown(c.dispose);

    // Push LampPickerSheet onto a real Navigator route so that
    // Navigator.pop() works and tiles stay inside the 800×600 viewport.
    // context.go() throws without a GoRouter — the try/catch inside
    // _InventoryLampTile._onTap absorbs it; we only assert on provider
    // state + pop.
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: MaterialApp(
        home: Builder(
          builder: (ctx) => Scaffold(
            body: TextButton(
              onPressed: () => Navigator.push<void>(
                ctx,
                MaterialPageRoute(
                  builder: (_) => const Scaffold(
                    body: LampPickerSheet(currentLampId: 'a'),
                  ),
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    ));
    await tester.tap(find.text('open'));
    await settle(tester);

    expect(find.text('jacko'), findsOneWidget);
    expect(find.text('melonie'), findsOneWidget);

    await tester.tap(find.text('melonie'));
    await settle(tester);

    expect(find.text('jacko'), findsNothing); // route popped
    expect(await c.read(activeLampNotifierProvider.future), 'b');
  });

  testWidgets('hides "Other nearby lamps" header when none', (tester) async {
    final c = await withInventory(const [
      InventoryLamp(id: 'a', name: 'jacko'),
    ]);
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: LampPickerSheet(currentLampId: 'a')),
      ),
    ));
    await settle(tester);
    expect(find.text('Other nearby lamps'), findsNothing);
  });

  testWidgets('sticky "+ Add a lamp" footer is always present', (tester) async {
    final c = await withInventory(const []);
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: LampPickerSheet(currentLampId: '')),
      ),
    ));
    await settle(tester);
    expect(find.text('Add a lamp'), findsOneWidget);
  });

  testWidgets('shows "Other nearby lamps" section for unknown nearby lamps',
      (tester) async {
    final c = await withInventory(const [
      InventoryLamp(id: 'a', name: 'jacko'),
    ]);
    addTearDown(c.dispose);

    // Inject a nearby lamp that is not in inventory by setting state directly.
    c.read(nearbyLampsNotifierProvider.notifier).state = [
      const NearbyLamp(
        id: 'unknown-1',
        name: "alice's lamp",
        rssi: -60,
        serviceUuids: [],
        baseRgb: 0xFF0000,
        shadeRgb: 0x00FF00,
        lastSeenEpochMs: 0,
      ),
    ];

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: LampPickerSheet(currentLampId: 'a')),
      ),
    ));
    await settle(tester);

    expect(find.text('Other nearby lamps'), findsOneWidget);
    expect(find.text("alice's lamp"), findsOneWidget);
    // Should show "add" pill (not factory default)
    expect(find.text('add'), findsOneWidget);
  });

  testWidgets('factory-default nearby lamp shows "adopt" pill', (tester) async {
    final c = await withInventory(const []);
    addTearDown(c.dispose);

    // Factory default: name='standard', baseRgb=0x300783, shadeRgb=0x000000
    c.read(nearbyLampsNotifierProvider.notifier).state = [
      const NearbyLamp(
        id: 'factory-1',
        name: 'standard',
        rssi: -65,
        serviceUuids: [],
        baseRgb: 0x300783,
        shadeRgb: 0x000000,
        lastSeenEpochMs: 0,
      ),
    ];

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: Scaffold(body: LampPickerSheet(currentLampId: '')),
      ),
    ));
    await settle(tester);

    expect(find.text('adopt'), findsOneWidget);
    expect(find.text('add'), findsNothing);
  });

  testWidgets('tapping current inventory tile pops without error',
      (tester) async {
    final c = await withInventory(const [
      InventoryLamp(id: 'a', name: 'jacko'),
    ]);
    addTearDown(c.dispose);

    // Use Navigator.push (same approach as the non-active test) so pop works.
    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: MaterialApp(
        home: Builder(
          builder: (ctx) => Scaffold(
            body: TextButton(
              onPressed: () => Navigator.push<void>(
                ctx,
                MaterialPageRoute(
                  builder: (_) => const Scaffold(
                    body: LampPickerSheet(currentLampId: 'a'),
                  ),
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    ));
    await tester.tap(find.text('open'));
    await settle(tester);

    expect(find.text('active'), findsOneWidget);
    await tester.tap(find.text('jacko'));
    await settle(tester);

    // Current tile just pops — no router call, no errors.
    expect(find.text('active'), findsNothing);
  });
}
