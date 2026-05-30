import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';

class BrightnessCard extends StatelessWidget {
  const BrightnessCard({
    super.key,
    required this.value,
    required this.onChanged,
  });

  final int value;
  final ValueChanged<int> onChanged;

  /// Interpolates the slider thumb black → amber-gold → white as brightness
  /// goes 0 → 50 → 100 %. Ported from the prior Vue app's BrightnessSlider
  /// (`software/lamp-app/src/components/BrightnessSlider.vue:27-48`). The
  /// amber midpoint visually signals "warm light, partially dimmed."
  static Color _thumbColorFor(int pct) {
    if (pct <= 50) {
      return Color.lerp(
          BrandColors.midnightBlack, BrandColors.amberGold, pct / 50)!;
    }
    return Color.lerp(
        BrandColors.amberGold, BrandColors.lampWhite, (pct - 50) / 50)!;
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.04),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              const Text(
                'Brightness',
                style: TextStyle(
                  color: BrandColors.lampWhite,
                  fontSize: 14,
                  fontWeight: FontWeight.w600,
                  letterSpacing: 0.4,
                ),
              ),
              const Spacer(),
              Text(
                '$value%',
                style: const TextStyle(
                  color: BrandColors.fogGrey,
                  fontSize: 12,
                ),
              ),
            ],
          ),
          SliderTheme(
            data: SliderTheme.of(context).copyWith(
              thumbColor: _thumbColorFor(value),
              // Track stays on the brand palette; only the thumb morphs so
              // the cue is unmistakable but the chrome stays consistent.
              activeTrackColor: BrandColors.amberGold.withValues(alpha: 0.5),
            ),
            child: Slider(
              min: 0,
              max: 100,
              divisions: 100,
              value: value.toDouble(),
              onChanged: (v) => onChanged(v.toInt()),
            ),
          ),
        ],
      ),
    );
  }
}
