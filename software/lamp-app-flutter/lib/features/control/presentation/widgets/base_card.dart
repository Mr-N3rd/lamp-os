import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';

class BaseCard extends StatelessWidget {
  const BaseCard({
    super.key,
    required this.colors,
    required this.activeIndex,
    required this.onTap,
  });

  final List<LampColor> colors;
  final int activeIndex;
  final VoidCallback onTap;

  /// Returns a gradient-safe list: LinearGradient requires ≥2 stops, so a
  /// single-color list is duplicated, and an empty list falls back to black.
  List<Color> _gradientColors() {
    if (colors.isEmpty) return const [Colors.black, Colors.black];
    final swatches = colors.map((c) => c.toSwatch()).toList();
    if (swatches.length == 1) return [swatches.first, swatches.first];
    return swatches;
  }

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(12),
      child: Container(
        margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
        padding: const EdgeInsets.all(16),
        decoration: BoxDecoration(
          color: Colors.white.withValues(alpha: 0.04),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
        ),
        child: Row(
          children: [
            Container(
              width: 28,
              height: 60,
              decoration: BoxDecoration(
                borderRadius: BorderRadius.circular(6),
                gradient: LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: _gradientColors(),
                ),
              ),
            ),
            const SizedBox(width: 12),
            Column(
              children: [
                for (var i = 0; i < colors.length; i++)
                  Container(
                    margin: const EdgeInsets.only(bottom: 4),
                    width: 16,
                    height: 16,
                    decoration: BoxDecoration(
                      color: colors[i].toSwatch(),
                      shape: BoxShape.circle,
                      border: Border.all(
                        color: i == activeIndex
                            ? BrandColors.glowPink
                            : Colors.white.withValues(alpha: 0.12),
                        width: i == activeIndex ? 2 : 1,
                      ),
                    ),
                  ),
              ],
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Base',
                    style: TextStyle(
                      color: BrandColors.lampWhite,
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                      letterSpacing: 0.4,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    'Base · ${colors.length} stops',
                    style: const TextStyle(
                      color: BrandColors.fogGrey,
                      fontSize: 12,
                    ),
                  ),
                ],
              ),
            ),
            const Icon(Icons.chevron_right, color: BrandColors.slateGrey),
          ],
        ),
      ),
    );
  }
}
