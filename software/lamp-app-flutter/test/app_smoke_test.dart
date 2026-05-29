import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/app.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  testWidgets('app boots and shows onboarding when inventory empty',
      (tester) async {
    SharedPreferences.setMockInitialValues({});
    await tester.pumpWidget(const ProviderScope(child: LampApp()));
    await tester.pumpAndSettle();
    expect(find.text('No lamps yet'), findsOneWidget);
  });
}
