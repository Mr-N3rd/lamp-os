import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/control/presentation/knockout_screen.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';

import '../../../_support/seed.dart';

const _devId = 'lamp-x';

Future<void> _seed(InMemoryBleClient ble) => seedControlBle(
      ble,
      deviceId: _devId,
      name: 'test',
      basePx: 3,
      baseKnockoutJson: '[{"p":1,"b":50}]',
    );

Future<ProviderContainer> _withState() async {
  SharedPreferences.setMockInitialValues({});
  final ble = InMemoryBleClient();
  await _seed(ble);
  final c = ProviderContainer(
    overrides: [bleClientProvider.overrideWithValue(ble)],
  );
  await c.read(inventoryNotifierProvider.future);
  await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
        id: _devId,
        name: 'jacko',
        controlPassword: 'secret',
      ));
  await c.read(controlNotifierProvider(_devId).future);
  return c;
}

void main() {
  testWidgets('renders one row per LED with the right brightness label',
      (tester) async {
    final c = await _withState();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: KnockoutScreen(lampId: _devId),
      ),
    ));
    await tester.pumpAndSettle();

    // 3 rows for px = 3.
    expect(find.text('#0'), findsOneWidget);
    expect(find.text('#1'), findsOneWidget);
    expect(find.text('#2'), findsOneWidget);

    // Default rows show 100%, the seeded row shows 50%.
    expect(find.text('100%'), findsNWidgets(2));
    expect(find.text('50%'), findsOneWidget);
  });

  testWidgets('Reset all returns every pixel to 100%', (tester) async {
    final c = await _withState();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: KnockoutScreen(lampId: _devId),
      ),
    ));
    await tester.pumpAndSettle();

    await tester.tap(find.text('Reset all'));
    await tester.pumpAndSettle();

    expect(find.text('100%'), findsNWidgets(3));
    expect(find.text('50%'), findsNothing);
  });

  testWidgets('title shows the lamp name', (tester) async {
    final c = await _withState();
    addTearDown(c.dispose);

    await tester.pumpWidget(UncontrolledProviderScope(
      container: c,
      child: const MaterialApp(
        home: KnockoutScreen(lampId: _devId),
      ),
    ));
    await tester.pumpAndSettle();
    expect(find.text('Pixel Knockout · jacko'), findsOneWidget);
  });
}
