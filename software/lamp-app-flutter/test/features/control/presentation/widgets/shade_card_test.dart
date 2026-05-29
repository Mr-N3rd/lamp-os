import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/shade_card.dart';

// NOTE: The live-preview and cancel-revert logic in ShadeCard._onTap is
// covered by ControlNotifier integration tests in T4. Widget-level bottom-sheet
// driving is intentionally omitted here to keep this test hermetic.

void main() {
  testWidgets('shows full RRGGBBWW hex when bpp = 4', (tester) async {
    const color = LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF);
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: ShadeCard(color: color, bpp: 4, onChanged: (_) {}),
      ),
    ));
    expect(find.text('Shade'), findsOneWidget);
    expect(find.text('#300783FF'), findsOneWidget);
  });

  testWidgets('trims hex to RRGGBB when bpp = 3', (tester) async {
    const color = LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF);
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: ShadeCard(color: color, bpp: 3, onChanged: (_) {}),
      ),
    ));
    expect(find.text('#300783'), findsOneWidget);
    expect(find.text('#300783FF'), findsNothing);
  });
}
