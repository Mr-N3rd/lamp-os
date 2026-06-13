import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/lamp_color_swatch.dart';

void main() {
  group('warmWhiteOpacity', () {
    test('zero W is always 0 opacity', () {
      const c = LampColor(r: 0, g: 0, b: 0, w: 0);
      expect(LampColorSwatch.warmWhiteOpacity(c), 0.0);
    });

    test('full W with RGB=0 is full opacity', () {
      const c = LampColor(r: 0, g: 0, b: 0, w: 255);
      expect(LampColorSwatch.warmWhiteOpacity(c), 1.0);
    });

    test('full W with RGB fully white has zero room → 0 opacity', () {
      const c = LampColor(r: 255, g: 255, b: 255, w: 255);
      expect(LampColorSwatch.warmWhiteOpacity(c), 0.0);
    });

    test('mid W with half-room is proportional', () {
      // 765/2 ≈ 382 room remaining when R+G+B = 383
      const c = LampColor(r: 255, g: 128, b: 0, w: 128);
      final v = LampColorSwatch.warmWhiteOpacity(c);
      // (128/255) * ((765-383)/765) ≈ 0.502 * 0.499 ≈ 0.250
      expect(v, closeTo(0.25, 0.02));
    });
  });

  testWidgets('renders the screen-blended composite as a single fill',
      (tester) async {
    // Post-2026-06-12: swatch was a two-Container Stack composing the W
    // overlay via default alpha blend. The blend was wrong (muddied
    // bright colors). Now it uses LampColor.blendedRgb's screen blend
    // and paints a single Container, matching the old Vue UI.
    const c = LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0x80);
    await tester.pumpWidget(const MaterialApp(
      home: Scaffold(body: LampColorSwatch(color: c)),
    ));
    expect(find.byType(LampColorSwatch), findsOneWidget);
    final container = tester.widget<Container>(
      find.descendant(
        of: find.byType(LampColorSwatch),
        matching: find.byType(Container),
      ),
    );
    final deco = container.decoration as BoxDecoration;
    final expected = c.toSwatch();
    expect(deco.color, expected);
  });

  testWidgets('roundedSquare shape uses BoxShape.rectangle', (tester) async {
    await tester.pumpWidget(const MaterialApp(
      home: Scaffold(
        body: LampColorSwatch(
          color: LampColor(r: 0x11, g: 0x22, b: 0x33, w: 0),
          shape: LampSwatchShape.roundedSquare,
          size: 56,
        ),
      ),
    ));
    final container = tester.widgetList<Container>(
        find.descendant(
          of: find.byType(LampColorSwatch),
          matching: find.byType(Container),
        )).first;
    final deco = container.decoration as BoxDecoration;
    expect(deco.shape, BoxShape.rectangle);
    expect(deco.borderRadius, isNotNull);
  });
}
