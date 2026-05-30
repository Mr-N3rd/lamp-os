import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';

/// A circular swatch that visualizes a [LampColor] — including the
/// separate warm-white LED's contribution. Ported from
/// `software/lamp-app/src/components/ColorPreview.vue:40-78`.
///
/// The W channel drives a `#FABB3E` (warm orange) overlay whose opacity is
/// proportional to W AND to how much "room" RGB still has. With RGB fully
/// saturated to white the overlay is invisible (no room left); with RGB
/// near black the overlay can take over the swatch.
class LampColorSwatch extends StatelessWidget {
  const LampColorSwatch({
    super.key,
    required this.color,
    this.size = 48,
    this.borderColor,
  });

  final LampColor color;
  final double size;
  final Color? borderColor;

  static const _warmWhiteHex = BrandColors.warmWhite;

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
    final overlayOpacity = warmWhiteOpacity(color);
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        border: Border.all(
          color: borderColor ?? Colors.white.withValues(alpha: 0.12),
        ),
      ),
      clipBehavior: Clip.antiAlias,
      child: Stack(
        fit: StackFit.expand,
        children: [
          Container(color: Color.fromARGB(0xFF, color.r, color.g, color.b)),
          Container(color: _warmWhiteHex.withValues(alpha: overlayOpacity)),
        ],
      ),
    );
  }
}
