import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/color_picker_sheet.dart';

void main() {
  testWidgets('opens without throwing when onLive is provided', (tester) async {
    // Give the test surface a phone-like size so flutter_colorpicker's internal
    // Row has enough width and doesn't overflow during layout.
    tester.view.physicalSize = const Size(1080, 1920);
    tester.view.devicePixelRatio = 3.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(MaterialApp(
      home: Builder(
        builder: (ctx) => Scaffold(
          body: Center(
            child: TextButton(
              onPressed: () => showColorPickerSheet(
                ctx,
                initial: const LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0),
                onLive: (_) {},
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    ));
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    expect(find.text('Pick a color'), findsOneWidget);
  });
}
