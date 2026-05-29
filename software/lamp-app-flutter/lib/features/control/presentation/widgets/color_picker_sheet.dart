import 'package:flutter/material.dart';
import 'package:flutter_colorpicker/flutter_colorpicker.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';

/// Opens a modal bottom sheet to pick a color, returning the chosen
/// [LampColor] or null if cancelled. The W byte of [initial] is carried
/// through untouched — the picker only edits R/G/B.
Future<LampColor?> showColorPickerSheet(
  BuildContext context, {
  required LampColor initial,
  String title = 'Pick a color',
}) {
  return showModalBottomSheet<LampColor>(
    context: context,
    isScrollControlled: true,
    backgroundColor: BrandColors.midnightBlack,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) => _ColorPickerSheet(initial: initial, title: title),
  );
}

class _ColorPickerSheet extends StatefulWidget {
  const _ColorPickerSheet({required this.initial, required this.title});
  final LampColor initial;
  final String title;

  @override
  State<_ColorPickerSheet> createState() => _ColorPickerSheetState();
}

class _ColorPickerSheetState extends State<_ColorPickerSheet> {
  late Color _picked = widget.initial.toSwatch();

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
                onColorChanged: (c) => setState(() => _picked = c),
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
