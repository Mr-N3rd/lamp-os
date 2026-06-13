import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';
import 'color_picker_sheet.dart';
import 'lamp_color_swatch.dart';

class ShadeCard extends StatelessWidget {
  const ShadeCard({
    super.key,
    required this.color,
    required this.bpp,
    required this.onChanged,
  });

  final LampColor color;

  /// 4 (RGBW) exposes Warm White in the picker; 3 (RGB-only) hides it
  /// and trims the displayed hex to `#RRGGBB`.
  final int bpp;

  final ValueChanged<LampColor> onChanged;

  Future<void> _onTap(BuildContext context) async {
    final original = color;
    final picked = await showColorPickerSheet(
      context,
      initial: color,
      title: 'Pick a shade color',
      bpp: bpp,
      onLive: onChanged, // every drag tick streams to the notifier
    );
    if (picked == null) {
      // User cancelled — revert the lamp to the color before picker opened.
      onChanged(original);
    } else {
      // Save tap: commit the final color (idempotent with last live update).
      onChanged(picked);
    }
  }

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: () => _onTap(context),
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
            LampColorSwatch(color: color, size: 56, shape: LampSwatchShape.roundedSquare, borderRadius: 14),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Shade',
                    style: TextStyle(
                      color: BrandColors.lampWhite,
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                      letterSpacing: 0.4,
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
