import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_svg/flutter_svg.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/lamp_preview.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  setUp(() => SharedPreferences.setMockInitialValues({}));

  Widget wrap(Widget child) =>
      ProviderScope(child: MaterialApp(home: Scaffold(body: child)));

  testWidgets('renders a SvgPicture after the template loads', (tester) async {
    await tester.pumpWidget(wrap(const LampPreview(
      deviceId: 'dev-1',
      shade: LampColor(r: 0xAB, g: 0xCD, b: 0xEF, w: 0xFF),
      baseColors: [
        LampColor(r: 0xFF, g: 0x00, b: 0x00, w: 0),
        LampColor(r: 0x00, g: 0xFF, b: 0x00, w: 0),
      ],
    )));
    await tester.pumpAndSettle();
    expect(find.byType(SvgPicture), findsOneWidget);
  });

  testWidgets(
      'single baseColor produces two identical stops (no division-by-zero)',
      (tester) async {
    await tester.pumpWidget(wrap(const LampPreview(
      deviceId: 'dev-1',
      shade: LampColor(r: 0xFF, g: 0xFF, b: 0xFF, w: 0),
      baseColors: [LampColor(r: 0x10, g: 0x20, b: 0x30, w: 0)],
    )));
    await tester.pumpAndSettle();
    expect(find.byType(SvgPicture), findsOneWidget);
  });

  testWidgets('empty baseColors falls back to black stops without throwing',
      (tester) async {
    await tester.pumpWidget(wrap(const LampPreview(
      deviceId: 'dev-1',
      shade: LampColor(r: 0, g: 0, b: 0, w: 0),
      baseColors: [],
    )));
    await tester.pumpAndSettle();
    expect(find.byType(SvgPicture), findsOneWidget);
  });
}
