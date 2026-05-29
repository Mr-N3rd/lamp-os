import 'package:flutter/material.dart';
import 'package:flutter_colorpicker/flutter_colorpicker.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';

/// Opens a modal bottom sheet to pick a color, returning the chosen
/// [LampColor] or null if cancelled. The picker controls R/G/B only and
/// emits results with W = 0, so the strip's separate white LED doesn't
/// wash out the chosen color (the shade default of #000000FF has W=255
/// which would otherwise dominate visually).
///
/// If [onLive] is provided it is called on every drag tick with the
/// current color so callers can stream realtime previews to the hardware.
/// Cancel still returns null; Save still returns the final color.
Future<LampColor?> showColorPickerSheet(
  BuildContext context, {
  required LampColor initial,
  String title = 'Pick a color',
  ValueChanged<LampColor>? onLive,
}) {
  return showModalBottomSheet<LampColor>(
    context: context,
    isScrollControlled: true,
    backgroundColor: BrandColors.midnightBlack,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) =>
        _ColorPickerSheet(initial: initial, title: title, onLive: onLive),
  );
}

class _ColorPickerSheet extends StatefulWidget {
  const _ColorPickerSheet({
    required this.initial,
    required this.title,
    this.onLive,
  });
  final LampColor initial;
  final String title;
  final ValueChanged<LampColor>? onLive;

  @override
  State<_ColorPickerSheet> createState() => _ColorPickerSheetState();
}

class _ColorPickerSheetState extends State<_ColorPickerSheet> {
  late Color _picked = widget.initial.toSwatch();

  void _onColorChanged(Color c) {
    setState(() => _picked = c);
    widget.onLive?.call(
      LampColor(
        r: (_picked.r * 255).round(),
        g: (_picked.g * 255).round(),
        b: (_picked.b * 255).round(),
        w: 0,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: EdgeInsets.only(
        bottom: MediaQuery.of(context).viewInsets.bottom,
      ),
      child: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Text(
                widget.title,
                style: const TextStyle(
                  color: BrandColors.lampWhite,
                  fontSize: 16,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 12),
              ColorPicker(
                pickerColor: _picked,
                onColorChanged: _onColorChanged,
                pickerAreaHeightPercent: 0.6,
                enableAlpha: false,
                labelTypes: const [],
                hexInputBar: true,
              ),
              const SizedBox(height: 12),
              Row(
                children: [
                  TextButton(
                    onPressed: () => Navigator.pop(context),
                    child: const Text('Cancel'),
                  ),
                  const Spacer(),
                  FilledButton(
                    onPressed: () => Navigator.pop(
                      context,
                      widget.initial.withRgb(
                        r: (_picked.r * 255).round(),
                        g: (_picked.g * 255).round(),
                        b: (_picked.b * 255).round(),
                      ),
                    ),
                    child: const Text('Save'),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}
