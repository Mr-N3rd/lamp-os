import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_svg/flutter_svg.dart';

import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/lamp_preview.dart';

void main() {
  // Make rootBundle available in tests.
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('renders a SvgPicture after the template loads', (tester) async {
    await tester.pumpWidget(const MaterialApp(
      home: Scaffold(
        body: LampPreview(
          shade: LampColor(r: 0xAB, g: 0xCD, b: 0xEF, w: 0xFF),
          baseColors: [
            LampColor(r: 0xFF, g: 0x00, b: 0x00, w: 0),
            LampColor(r: 0x00, g: 0xFF, b: 0x00, w: 0),
          ],
        ),
      ),
    ));

    // First frame: async rootBundle load may not be resolved yet —
    // the widget shows an empty SizedBox at this point.
    await tester.pump();

    // Drain remaining microtasks and timers so rootBundle.loadString resolves.
    await tester.pumpAndSettle();

    // After settling the SVG picture should be present.
    expect(find.byType(SvgPicture), findsOneWidget);
  });

  testWidgets('single baseColor produces two identical stops (no division-by-zero)',
      (tester) async {
    await tester.pumpWidget(const MaterialApp(
      home: Scaffold(
        body: LampPreview(
          shade: LampColor(r: 0xFF, g: 0xFF, b: 0xFF, w: 0),
          baseColors: [LampColor(r: 0x10, g: 0x20, b: 0x30, w: 0)],
        ),
      ),
    ));
    await tester.pumpAndSettle();
    expect(find.byType(SvgPicture), findsOneWidget);
  });

  testWidgets('empty baseColors falls back to black stops without throwing',
      (tester) async {
    await tester.pumpWidget(const MaterialApp(
      home: Scaffold(
        body: LampPreview(
          shade: LampColor(r: 0, g: 0, b: 0, w: 0),
          baseColors: [],
        ),
      ),
    ));
    await tester.pumpAndSettle();
    expect(find.byType(SvgPicture), findsOneWidget);
  });
}
