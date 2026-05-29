abstract class AppRoutes {
  static const onboarding = '/onboarding';
  static const addLamp = '/onboarding/add';
  static const lampPicker = '/lamp-picker';
  static const info = '/info';

  static String lamp(String id) => '/lamp/$id';
  static String control(String id) => '/lamp/$id/control';
  static String expressions(String id) => '/lamp/$id/expressions';
  static String expressionEditor(String id, String type, int target) =>
      '/lamp/$id/expressions/$type/$target';
  static String knockout(String id) => '/lamp/$id/control/knockout';
  static String setup(String id) => '/lamp/$id/setup';
}
