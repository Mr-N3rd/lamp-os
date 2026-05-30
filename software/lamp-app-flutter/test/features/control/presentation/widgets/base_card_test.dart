import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/base_card.dart';

void main() {
  testWidgets('renders the "Base" label and no "stops" subtitle',
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
    expect(find.text('Base'), findsOneWidget);
    expect(find.textContaining('stops'), findsNothing);
  });

  testWidgets('does not render per-color circle chips', (tester) async {
    const colors = [
      LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0),
      LampColor(r: 0xFF, g: 0x00, b: 0x00, w: 0),
      LampColor(r: 0x00, g: 0xFF, b: 0x00, w: 0),
    ];
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BaseCard(colors: colors, activeIndex: 0, onTap: () {}),
      ),
    ));
    // Count Containers with BoxShape.circle inside the BaseCard subtree.
    // The base card's only Container is the gradient ribbon (rectangle).
    // No circle Containers should exist for any of the 3 colors.
    final circles = tester
        .widgetList<Container>(find.descendant(
            of: find.byType(BaseCard), matching: find.byType(Container)))
        .where((c) {
      final d = c.decoration;
      return d is BoxDecoration && d.shape == BoxShape.circle;
    }).toList();
    expect(circles, isEmpty);
  });

  testWidgets('still renders the gradient ribbon container', (tester) async {
    const colors = [
      LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0),
      LampColor(r: 0xFF, g: 0x00, b: 0x00, w: 0),
    ];
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BaseCard(colors: colors, activeIndex: 0, onTap: () {}),
      ),
    ));
    // The gradient ribbon is the only Container inside BaseCard with a
    // LinearGradient decoration.
    final hasGradient = tester
        .widgetList<Container>(find.descendant(
            of: find.byType(BaseCard), matching: find.byType(Container)))
        .where((c) {
      final d = c.decoration;
      return d is BoxDecoration && d.gradient is LinearGradient;
    }).toList();
    expect(hasGradient, isNotEmpty);
  });
}
