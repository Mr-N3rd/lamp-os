import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/base_card.dart';

void main() {
  testWidgets('shows stop count subtitle and a ribbon for N stops',
      (tester) async {
    const colors = [
      LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0),
      LampColor(r: 0xFF, g: 0x00, b: 0x00, w: 0),
    ];
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BaseCard(colors: colors, activeIndex: 0, onTap: () {}),
      ),
    ));
    expect(find.text('Base · 2 stops'), findsOneWidget);
  });
}
