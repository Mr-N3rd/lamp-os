import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/presentation/widgets/connecting_view.dart';
import 'package:lamp_app/features/lamp_shell/presentation/lamp_shell.dart';

void main() {
  testWidgets('renders Control by default, switches to Expressions on tap',
      (tester) async {
    await tester.pumpWidget(const ProviderScope(
      child: MaterialApp(
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
}
