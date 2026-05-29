import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/shade_card.dart';

void main() {
  testWidgets('renders title, hex label, and swatch', (tester) async {
    const color = LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF);
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: ShadeCard(color: color, onChanged: (_) {}),
      ),
    ));
    expect(find.text('Shade'), findsOneWidget);
    expect(find.text('#300783FF'), findsOneWidget);
  });
}
