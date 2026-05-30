import 'package:flutter/painting.dart' show Color;

/// A lamp color carries an explicit white channel alongside RGB. The firmware
/// serializes these as `#RRGGBBWW` hex strings on every section payload.
class LampColor {
  const LampColor({
    required this.r,
    required this.g,
    required this.b,
    required this.w,
  });

  final int r;
  final int g;
  final int b;
  final int w;

  factory LampColor.fromHex(String input) {
    var s = input.startsWith('#') ? input.substring(1) : input;
    if (s.length != 8) {
      throw FormatException('LampColor.fromHex expects 8 hex chars, got "$input"');
    }
    return LampColor(
      r: int.parse(s.substring(0, 2), radix: 16),
      g: int.parse(s.substring(2, 4), radix: 16),
      b: int.parse(s.substring(4, 6), radix: 16),
      w: int.parse(s.substring(6, 8), radix: 16),
    );
  }

  /// Lenient parser used by the color-picker hex input. Accepts `#RRGGBB`
  /// (W defaults to 0) or `#RRGGBBWW`, leading `#` optional, case-insensitive.
  /// Returns null on any parse failure so the caller can surface an error
  /// state without a try/catch.
  static LampColor? tryFromHex(String input) {
    var s = input.trim();
    if (s.startsWith('#')) s = s.substring(1);
    if (s.length != 6 && s.length != 8) return null;
    final r = int.tryParse(s.substring(0, 2), radix: 16);
    final g = int.tryParse(s.substring(2, 4), radix: 16);
    final b = int.tryParse(s.substring(4, 6), radix: 16);
    if (r == null || g == null || b == null) return null;
    if (s.length == 6) return LampColor(r: r, g: g, b: b, w: 0);
    final w = int.tryParse(s.substring(6, 8), radix: 16);
    if (w == null) return null;
    return LampColor(r: r, g: g, b: b, w: w);
  }

  String toHex() {
    String h(int v) => v.toRadixString(16).padLeft(2, '0').toUpperCase();
    return '#${h(r)}${h(g)}${h(b)}${h(w)}';
  }

  /// Convert RGB portion to a Flutter [Color] for swatch/preview rendering.
  /// The W byte is dropped — it only ever drives the physical strip's white
  /// channel, never the on-screen swatch.
  Color toSwatch() => Color.fromARGB(0xFF, r, g, b);

  LampColor withRgb({required int r, required int g, required int b}) =>
      LampColor(r: r, g: g, b: b, w: w);

  @override
  bool operator ==(Object other) =>
      other is LampColor &&
      other.r == r &&
      other.g == g &&
      other.b == b &&
      other.w == w;

  @override
  int get hashCode => Object.hash(r, g, b, w);

  @override
  String toString() => 'LampColor(${toHex()})';
}
