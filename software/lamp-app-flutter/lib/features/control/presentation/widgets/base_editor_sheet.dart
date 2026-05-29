import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../domain/lamp_color.dart';
import 'color_picker_sheet.dart';

class BaseEditorSheet extends StatelessWidget {
  const BaseEditorSheet({
    super.key,
    required this.colors,
    required this.activeIndex,
    required this.onColorsChanged,
    required this.onActiveChanged,
  });

  final List<LampColor> colors;
  final int activeIndex;
  final ValueChanged<List<LampColor>> onColorsChanged;
  final ValueChanged<int> onActiveChanged;

  Future<void> _editStop(BuildContext context, int i) async {
    final picked = await showColorPickerSheet(
      context,
      initial: colors[i],
      title: 'Stop ${i + 1}',
    );
    if (picked != null) {
      final next = [...colors];
      next[i] = picked;
      onColorsChanged(next);
    }
  }

  void _removeStop(int i) {
    if (colors.length <= 1) return;
    final next = [...colors]..removeAt(i);
    onColorsChanged(next);
    if (activeIndex >= next.length) onActiveChanged(next.length - 1);
  }

  void _addStop() {
    if (colors.length >= 5) return;
    onColorsChanged([
      ...colors,
      const LampColor(r: 0xFF, g: 0xFF, b: 0xFF, w: 0),
    ]);
  }

  void _reorder(int oldIndex, int newIndex) {
    final next = [...colors];
    final picked = next.removeAt(oldIndex);
    next.insert(newIndex, picked);
    onColorsChanged(next);
    if (activeIndex == oldIndex) {
      onActiveChanged(newIndex);
    }
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            const Row(
              children: [
                Text(
                  'Base gradient',
                  style: TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 16,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                Spacer(),
              ],
            ),
            const SizedBox(height: 12),
            Expanded(
              child: ReorderableListView.builder(
                itemCount: colors.length,
                onReorderItem: _reorder,
                buildDefaultDragHandles: false,
                itemBuilder: (ctx, i) {
                  final stop = colors[i];
                  return ListTile(
                    key: ValueKey('stop-$i'),
                    onTap: () => onActiveChanged(i),
                    leading: ReorderableDragStartListener(
                      index: i,
                      child: const Icon(Icons.drag_indicator,
                          color: BrandColors.slateGrey),
                    ),
                    title: Row(
                      children: [
                        GestureDetector(
                          onTap: () => _editStop(context, i),
                          child: Container(
                            width: 28,
                            height: 28,
                            decoration: BoxDecoration(
                              color: stop.toSwatch(),
                              shape: BoxShape.circle,
                              border: Border.all(
                                color: i == activeIndex
                                    ? BrandColors.glowPink
                                    : Colors.white.withValues(alpha: 0.12),
                                width: i == activeIndex ? 2 : 1,
                              ),
                            ),
                          ),
                        ),
                        const SizedBox(width: 12),
                        Text(
                          '#${stop.toHex().substring(1, 7)}',
                          style: const TextStyle(
                            color: BrandColors.fogGrey,
                            fontSize: 12,
                            fontFamily: 'monospace',
                          ),
                        ),
                      ],
                    ),
                    trailing: IconButton(
                      icon: const Icon(Icons.close,
                          color: BrandColors.slateGrey),
                      onPressed: colors.length <= 1
                          ? null
                          : () => _removeStop(i),
                    ),
                  );
                },
              ),
            ),
            if (colors.length < 5)
              TextButton(
                onPressed: _addStop,
                child: const Text('+ Add stop'),
              ),
          ],
        ),
      ),
    );
  }
}

Future<void> showBaseEditorSheet(
  BuildContext context, {
  required List<LampColor> colors,
  required int activeIndex,
  required ValueChanged<List<LampColor>> onColorsChanged,
  required ValueChanged<int> onActiveChanged,
}) {
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    backgroundColor: BrandColors.midnightBlack,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) => FractionallySizedBox(
      heightFactor: 0.9,
      child: BaseEditorSheet(
        colors: colors,
        activeIndex: activeIndex,
        onColorsChanged: onColorsChanged,
        onActiveChanged: onActiveChanged,
      ),
    ),
  );
}
