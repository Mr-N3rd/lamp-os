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

  /// Blend the warm-white channel into RGB for on-screen rendering.
  /// Mirrors the Vue ColorPreview algorithm — same math as the
  /// existing `LampColorSwatch` two-layer overlay, just collapsed
  /// to one Color so callers that paint a single fill (BaseCard
  /// stop dots, LampPreview SVG fills, etc.) don't have to layer.
  ///
  /// Algorithm:
  ///   - tint = #FABB3E (matches BrandColors.warmWhite)
  ///   - opacity = (W / 255) × (headroom / 765)
  ///       where headroom = 765 − (R+G+B)
  /// High RGB → no headroom, no warm overlay (LED clips anyway).
  /// Low RGB + high W → strong warm glow.
  /// Pure warm-white (W=255, RGB=0) → fully tinted swatch.
  ({int r, int g, int b}) get blendedRgb {
    // Warm-white sRGB approximation. Matches BrandColors.warmWhite.
    // Hardcoded here so the domain layer doesn't depend on theme.
    const tintR = 0xFA;
    const tintG = 0xBB;
    const tintB = 0x3E;
    final headroom = 765 - (r + g + b);
    if (headroom <= 0 || w == 0) return (r: r, g: g, b: b);
    final opacity = (w / 255) * (headroom / 765);
    final inv = 1 - opacity;
    return (
      r: (r * inv + tintR * opacity).round().clamp(0, 255),
      g: (g * inv + tintG * opacity).round().clamp(0, 255),
      b: (b * inv + tintB * opacity).round().clamp(0, 255),
    );
  }

  /// Flutter [Color] with the warm-white channel blended in.
  /// Phone screens can't reproduce the physical W LED directly so
  /// we approximate it with a warm-tone overlay on RGB.
  Color toSwatch() {
    final c = blendedRgb;
    return Color.fromARGB(0xFF, c.r, c.g, c.b);
  }

  /// `RRGGBB` hex (no leading `#`) of the W-blended color. Used by
  /// the SVG color substitution in LampPreview.
  String toRgbHex() {
    String h(int v) => v.toRadixString(16).padLeft(2, '0').toUpperCase();
    final c = blendedRgb;
    return '${h(c.r)}${h(c.g)}${h(c.b)}';
  }

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
