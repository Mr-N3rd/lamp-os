import 'package:flutter/material.dart';

import '../../domain/lamp_color.dart';

/// A swatch that visualizes a [LampColor] — including the separate
/// warm-white LED's contribution. Composites the W tint via SCREEN
/// blend (matching the old Vue ColorPreview's CSS
/// `mix-blend-mode: screen`) so bright colors retain their brightness
/// and gain warm glow, instead of being muddied toward the tint as
/// they would be under default alpha blend.
///
/// The actual screen-blend math lives in [LampColor.blendedRgb]; this
/// widget is just a colored box that uses it via [LampColor.toSwatch].

/// Two-shape variants of [LampColorSwatch]. The default is [circle] so
/// existing call-sites stay unchanged; the [roundedSquare] variant is
/// used by [ShadeCard] to visually rhyme with [BaseCard].
enum LampSwatchShape { circle, roundedSquare }

class LampColorSwatch extends StatelessWidget {
  const LampColorSwatch({
    super.key,
    required this.color,
    this.size = 48,
    this.borderColor,
    this.shape = LampSwatchShape.circle,
    this.borderRadius = 14,
  });

  final LampColor color;
  final double size;
  final Color? borderColor;
  final LampSwatchShape shape;
  final double borderRadius;

  /// `(W / 255) * (availableRoom / 765)`, clamped to [0, 1].
  /// Exposed for unit tests.
  static double warmWhiteOpacity(LampColor c) {
    final room = 765 - (c.r + c.g + c.b);
    if (room <= 0) return 0;
    final wwPct = c.w / 255.0;
    final roomPct = room / 765.0;
    return (wwPct * roomPct).clamp(0.0, 1.0);
  }

  @override
  Widget build(BuildContext context) {
    final isCircle = shape == LampSwatchShape.circle;
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        // Use the screen-blended composite from LampColor — matches the
        // old Vue ColorPreview component visually (which used CSS
        // mix-blend-mode: screen to overlay the W tint). The previous
        // two-Container Stack used the default srcOver alpha blend,
        // which muddied bright colors toward the warm tint instead of
        // additively brightening them.
        color: color.toSwatch(),
        shape: isCircle ? BoxShape.circle : BoxShape.rectangle,
        borderRadius:
            isCircle ? null : BorderRadius.circular(borderRadius),
        border: Border.all(
          color: borderColor ?? Colors.white.withValues(alpha: 0.12),
        ),
      ),
    );
  }
}
