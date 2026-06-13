import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/routing/routes.dart';

void main() {
  group('AppRoutes', () {
    test('static literals', () {
      expect(AppRoutes.onboarding, '/onboarding');
      expect(AppRoutes.addLamp, '/onboarding/add');
      expect(AppRoutes.lampPicker, '/lamp-picker');
      expect(AppRoutes.info, '/info');
    });

    test('parameterized builders', () {
      expect(AppRoutes.lamp('abc'), '/lamp/abc');
      expect(AppRoutes.control('abc'), '/lamp/abc/control');
      expect(AppRoutes.expressions('abc'), '/lamp/abc/expressions');
      expect(AppRoutes.expressionEditor('abc', 'e1'),
          '/lamp/abc/expressions/e1');
      expect(AppRoutes.knockout('abc'), '/lamp/abc/control/knockout');
      expect(AppRoutes.setup('abc'), '/lamp/abc/setup');
    });
  });
}
