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
