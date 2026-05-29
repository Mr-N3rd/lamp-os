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
  });

  final String ssid;
  final String password;
  final int brightness;

  factory HomeSection.fromJson(Map<String, dynamic> json) => HomeSection(
        ssid: (json['ssid'] as String?) ?? '',
        password: (json['password'] as String?) ?? '',
        brightness: (json['brightness'] as num?)?.toInt() ?? 60,
      );
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
