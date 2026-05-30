import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/lamp_color_swatch.dart';
import 'package:lamp_app/features/control/presentation/widgets/shade_card.dart';

void main() {
  testWidgets('renders the "Shade" label and no hex string', (tester) async {
    const color = LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF);
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: ShadeCard(color: color, bpp: 4, onChanged: (_) {}),
      ),
    ));
    expect(find.text('Shade'), findsOneWidget);
    expect(find.textContaining('#'), findsNothing);
    expect(find.textContaining('300783'), findsNothing);
  });

  testWidgets('uses a rounded-square swatch', (tester) async {
    const color = LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF);
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: ShadeCard(color: color, bpp: 4, onChanged: (_) {}),
      ),
    ));
    final swatch = tester.widget<LampColorSwatch>(
        find.byType(LampColorSwatch));
    expect(swatch.shape, LampSwatchShape.roundedSquare);
  });
}
