import 'lamp_color.dart';

/// CHAR_LAMP_SECTION payload, see firmware Config::asLampJson.
class LampSection {
  const LampSection({
    required this.name,
    required this.brightness,
    required this.advancedEnabled,
  });

  final String name;
  final int brightness;
  final bool advancedEnabled;

  factory LampSection.fromJson(Map<String, dynamic> json) => LampSection(
        name: (json['name'] as String?) ?? '',
        brightness: (json['brightness'] as num?)?.toInt() ?? 100,
        advancedEnabled: json['advancedEnabled'] as bool? ?? false,
      );
}

/// CHAR_BASE_SECTION payload, see firmware Config::asBaseJson.
class BaseSection {
  const BaseSection({
    required this.px,
    required this.ac,
    required this.bpp,
    required this.colors,
    required this.knockout,
  });

  final int px;
  final int ac;
  final int bpp;
  final List<LampColor> colors;

  /// Per-LED brightness overrides (0..100). Indices absent from the map use
  /// the default 100 %. Empty map = all LEDs at full brightness. Stored as a
  /// `Map<int, int>` rather than a list for O(1) lookup and small memory.
  final Map<int, int> knockout;

  factory BaseSection.fromJson(Map<String, dynamic> json) {
    final knockoutList = (json['knockout'] as List?) ?? const [];
    final knockoutMap = <int, int>{
      for (final entry in knockoutList.cast<Map<String, dynamic>>())
        if (entry['p'] is num && entry['b'] is num)
          (entry['p'] as num).toInt(): (entry['b'] as num).toInt(),
    };
    return BaseSection(
      px: (json['px'] as num?)?.toInt() ?? 35,
      ac: (json['ac'] as num?)?.toInt() ?? 0,
      bpp: (json['bpp'] as num?)?.toInt() ?? 4,
      colors: ((json['colors'] as List?) ?? const [])
          .map((e) => LampColor.fromHex(e as String))
          .toList(),
      knockout: knockoutMap,
    );
  }
}

/// CHAR_SHADE_SECTION payload, see firmware Config::asShadeJson.
class ShadeSection {
  const ShadeSection({
    required this.px,
    required this.bpp,
    required this.colors,
  });

  final int px;
  final int bpp;
  final List<LampColor> colors;

  factory ShadeSection.fromJson(Map<String, dynamic> json) => ShadeSection(
        px: (json['px'] as num?)?.toInt() ?? 38,
        bpp: (json['bpp'] as num?)?.toInt() ?? 4,
        colors: ((json['colors'] as List?) ?? const [])
            .map((e) => LampColor.fromHex(e as String))
            .toList(),
      );
}

/// CHAR_HOME_SECTION payload. Password is masked as "********" on read
/// (firmware-side, defense-in-depth). Tests + UI should treat that string
/// as "a password is set; don't change it on save unless the user typed a
/// new one".
class HomeSection {
  const HomeSection({
    required this.ssid,
    required this.password,
    required this.brightness,
    required this.enabled,
  });

  final String ssid;
  final String password;
  final int brightness;

  /// Soft on/off for Home Mode. When false, the firmware keeps the
  /// stored SSID/password but does not bring up the Wi-Fi STA at boot
  /// or after BT disconnect — letting the user keep their credentials
  /// without the lamp acting as a home-mode device.
  ///
  /// Migration: firmwares saved before this field existed have no
  /// `enabled` key in their NVS JSON. Treat presence of a saved SSID
  /// as "user wanted home mode on" so existing lamps keep working.
  final bool enabled;

  factory HomeSection.fromJson(Map<String, dynamic> json) {
    final ssid = (json['ssid'] as String?) ?? '';
    return HomeSection(
      ssid: ssid,
      password: (json['password'] as String?) ?? '',
      brightness: (json['brightness'] as num?)?.toInt() ?? 60,
      enabled: (json['enabled'] as bool?) ?? ssid.isNotEmpty,
    );
  }
}

/// CHAR_MQTT_SECTION payload. Same password-masking caveat as HomeSection.
class MqttSection {
  const MqttSection({
    required this.enabled,
    required this.brokerHost,
    required this.brokerPort,
    required this.username,
    required this.password,
    required this.topicPrefix,
  });

  final bool enabled;
  final String brokerHost;
  final int brokerPort;
  final String username;
  final String password;
  final String topicPrefix;

  factory MqttSection.fromJson(Map<String, dynamic> json) => MqttSection(
        enabled: json['enabled'] as bool? ?? false,
        brokerHost: (json['brokerHost'] as String?) ?? '',
        brokerPort: (json['brokerPort'] as num?)?.toInt() ?? 1883,
        username: (json['username'] as String?) ?? '',
        password: (json['password'] as String?) ?? '',
        topicPrefix: (json['topicPrefix'] as String?) ?? '',
      );
}

/// A single expression configuration. CHAR_EXPRESSION_SECTION returns an array
/// of these; see firmware ExpressionConfig.
class ExpressionConfig {
  const ExpressionConfig({
    required this.type,
    required this.enabled,
    required this.colors,
    required this.intervalMin,
    required this.intervalMax,
    required this.target,
    required this.parameters,
  });

  final String type;
  final bool enabled;
  final List<LampColor> colors;
  final int intervalMin;
  final int intervalMax;
  final int target; // 1=shade, 2=base, 3=both
  final Map<String, int> parameters;

  static const _reservedKeys = {
    'type', 'enabled', 'colors', 'intervalMin', 'intervalMax', 'target',
  };

  factory ExpressionConfig.fromJson(Map<String, dynamic> json) {
    final params = <String, int>{};
    for (final e in json.entries) {
      if (_reservedKeys.contains(e.key)) continue;
      if (e.value is num) params[e.key] = (e.value as num).toInt();
    }
    return ExpressionConfig(
      type: json['type'] as String? ?? '',
      enabled: json['enabled'] as bool? ?? false,
      colors: ((json['colors'] as List?) ?? const [])
          .map((e) => LampColor.fromHex(e as String))
          .toList(),
      intervalMin: (json['intervalMin'] as num?)?.toInt() ?? 60,
      intervalMax: (json['intervalMax'] as num?)?.toInt() ?? 900,
      target: (json['target'] as num?)?.toInt() ?? 3,
      parameters: params,
    );
  }

  Map<String, dynamic> toJson() => {
        'type': type,
        'enabled': enabled,
        'colors': colors.map((c) => c.toHex()).toList(),
        'intervalMin': intervalMin,
        'intervalMax': intervalMax,
        'target': target,
        ...parameters,
      };
}

/// CHAR_EXPRESSION_SECTION payload — a JSON array of ExpressionConfig objects.
class ExpressionsSection {
  const ExpressionsSection({required this.expressions});

  final List<ExpressionConfig> expressions;

  factory ExpressionsSection.fromJson(List<dynamic> json) =>
      ExpressionsSection(
        expressions: json
            .cast<Map<String, dynamic>>()
            .map(ExpressionConfig.fromJson)
            .toList(),
      );
}
