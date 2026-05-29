import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/presentation/widgets/base_editor_sheet.dart';

void main() {
  testWidgets('renders one row per stop and the add-stop CTA when < 5',
      (tester) async {
    final colors = const [
      LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0),
      LampColor(r: 0xFF, g: 0x00, b: 0x00, w: 0),
    ];
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BaseEditorSheet(
          colors: colors,
          activeIndex: 0,
          onColorsChanged: (_) {},
          onActiveChanged: (_) {},
        ),
      ),
    ));
    expect(find.text('#300783'), findsOneWidget);
    expect(find.text('#FF0000'), findsOneWidget);
    expect(find.text('+ Add stop'), findsOneWidget);
  });

  testWidgets('hides the add-stop CTA at 5 stops', (tester) async {
    final colors = List.generate(
      5,
      (i) => LampColor(r: i * 20, g: 0, b: 0, w: 0),
    );
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BaseEditorSheet(
          colors: colors,
          activeIndex: 0,
          onColorsChanged: (_) {},
          onActiveChanged: (_) {},
        ),
      ),
    ));
    expect(find.text('+ Add stop'), findsNothing);
  });
}
