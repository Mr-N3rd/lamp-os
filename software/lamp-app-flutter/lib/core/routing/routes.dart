abstract class AppRoutes {
  static const onboarding = '/onboarding';
  static const addLamp = '/onboarding/add';
  static const lampPicker = '/lamp-picker';
  static const myLamps = '/lamps';

  static String lamp(String id) => '/lamp/$id';
  static String control(String id) => '/lamp/$id/control';
  static String btOnly(String id) => '/lamp/$id/bt-only';
  static String expressions(String id) => '/lamp/$id/expressions';
  static String addExpression(String id) => '/lamp/$id/expressions/new';
  static String expressionEditor(String id, String type, int target) =>
      '/lamp/$id/expressions/$type/$target';
  static String knockout(String id) => '/lamp/$id/setup/knockout';
  static String setup(String id) => '/lamp/$id/setup';
  static String info(String id) => '/lamp/$id/info';
  // Settings sub-panes (drilled into from the row list on the Setup tab).
  static String homeWifi(String id) => '/lamp/$id/setup/wifi';
  static String homeMode(String id) => '/lamp/$id/setup/home-mode';
  static String advancedLeds(String id) => '/lamp/$id/setup/advanced-leds';
}
