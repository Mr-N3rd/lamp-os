import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/presentation/widgets/brightness_card.dart';

void main() {
  testWidgets('renders the percentage label and slider', (tester) async {
    int? changed;
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: BrightnessCard(value: 42, onChanged: (v) => changed = v),
      ),
    ));
    expect(find.text('42%'), findsOneWidget);
    expect(find.byType(Slider), findsOneWidget);

    final slider = tester.widget<Slider>(find.byType(Slider));
    slider.onChanged!(60);
    expect(changed, 60);
  });
}
