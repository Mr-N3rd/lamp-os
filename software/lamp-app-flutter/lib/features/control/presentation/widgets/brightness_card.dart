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
          Slider(
            min: 0,
            max: 100,
            divisions: 100,
            value: value.toDouble(),
            onChanged: (v) => onChanged(v.toInt()),
          ),
        ],
      ),
    );
  }
}
