import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../application/control_notifier.dart';
import '../../domain/lamp_color.dart';
import 'color_picker_sheet.dart';
import 'lamp_color_swatch.dart';

/// Tiny slice of ControlState the shade card needs to render — used as
/// the `.select` projection so sibling state changes don't rebuild us.
class _ShadeSlice {
  const _ShadeSlice(this.color, this.bpp);
  final LampColor color;
  final int bpp;

  @override
  bool operator ==(Object other) =>
      other is _ShadeSlice && other.color == color && other.bpp == bpp;

  @override
  int get hashCode => Object.hash(color, bpp);
}

class ShadeCard extends ConsumerWidget {
  const ShadeCard({
    super.key,
    required this.lampId,
    this.onEditSessionChanged,
  });

  final String lampId;

  /// Fires `true` when the picker opens and `false` when it closes
  /// (Save, Cancel, dismiss). Wired into `ControlNotifier.setEditSession`
  /// upstream so the lamp can lock out wisp paints to the shade surface
  /// while the operator is actively picking. Optional — non-control-
  /// screen consumers (none today) can leave it null.
  final ValueChanged<bool>? onEditSessionChanged;

  Future<void> _onTap(BuildContext context, WidgetRef ref, LampColor color,
      int bpp) async {
    final notifier = ref.read(controlNotifierProvider(lampId).notifier);
    final original = color;
    onEditSessionChanged?.call(true);
    try {
      final picked = await showColorPickerSheet(
        context,
        initial: color,
        title: 'Pick a shade color',
        bpp: bpp,
        // every drag tick streams to the notifier
        onLive: notifier.setShadeColor,
      );
      if (picked == null) {
        notifier.setShadeColor(original);
      } else {
        notifier.setShadeColor(picked);
      }
    } finally {
      onEditSessionChanged?.call(false);
    }
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final slice = ref.watch(
      controlNotifierProvider(lampId).select((async) {
        final state = async.value;
        if (state == null) return const _ShadeSlice(LampColor.black, 4);
        final color = state.shade.colors.isEmpty
            ? LampColor.black
            : state.shade.colors.single;
        return _ShadeSlice(color, state.shade.bpp);
      }),
    );
    return InkWell(
      onTap: () => _onTap(context, ref, slice.color, slice.bpp),
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
            LampColorSwatch(
                color: slice.color,
                size: 56,
                shape: LampSwatchShape.roundedSquare,
                borderRadius: 14),
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
