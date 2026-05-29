import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../application/control_notifier.dart';
import '../../domain/lamp_color.dart';
import 'color_picker_sheet.dart';

class BaseEditorSheet extends ConsumerWidget {
  const BaseEditorSheet({super.key, required this.lampId});

  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final state = ref.watch(controlNotifierProvider(lampId)).value;
    if (state == null) return const SizedBox.shrink();

    final colors = state.base.colors;
    final activeIndex = state.base.ac;
    final notifier = ref.read(controlNotifierProvider(lampId).notifier);

    Future<void> editStop(int i) async {
      final original = [...colors];
      final picked = await showColorPickerSheet(
        context,
        initial: colors[i],
        title: 'Stop ${i + 1}',
        onLive: (live) {
          // Latest colors come from the notifier — read fresh each tick so
          // concurrent state changes (e.g. another stop edited in parallel)
          // don't get clobbered. In practice the picker is modal so this is
          // belt-and-suspenders, but it matches the realtime contract.
          final current = ref.read(controlNotifierProvider(lampId)).value;
          if (current == null) return;
          final next = [...current.base.colors];
          if (i >= next.length) return; // stop removed while picker open
          next[i] = live;
          notifier.setBaseColors(next);
        },
      );
      if (picked == null) {
        // Cancelled — restore the snapshot we took before the picker opened.
        notifier.setBaseColors(original);
      }
      // Save case: the last onLive tick already wrote the correct color; no
      // further action needed.
    }

    void removeStop(int i) {
      if (colors.length <= 1) return;
      final next = [...colors]..removeAt(i);
      notifier.setBaseColors(next);
      if (activeIndex >= next.length) notifier.setBaseAc(next.length - 1);
    }

    void addStop() {
      if (colors.length >= 5) return;
      notifier.setBaseColors([
        ...colors,
        const LampColor(r: 0xFF, g: 0xFF, b: 0xFF, w: 0),
      ]);
    }

    void reorder(int oldIndex, int newIndex) {
      final next = [...colors];
      final picked = next.removeAt(oldIndex);
      next.insert(newIndex, picked);
      notifier.setBaseColors(next);
      if (activeIndex == oldIndex) {
        notifier.setBaseAc(newIndex);
      }
    }

    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              children: [
                const Text(
                  'Base gradient',
                  style: TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 16,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const Spacer(),
                IconButton(
                  icon: const Icon(Icons.close, color: BrandColors.slateGrey),
                  onPressed: () => Navigator.pop(context),
                  tooltip: 'Close',
                ),
              ],
            ),
            const SizedBox(height: 12),
            Expanded(
              child: ReorderableListView.builder(
                itemCount: colors.length,
                onReorderItem: reorder,
                buildDefaultDragHandles: false,
                itemBuilder: (ctx, i) {
                  final stop = colors[i];
                  return ListTile(
                    key: ValueKey('stop-$i'),
                    onTap: () => notifier.setBaseAc(i),
                    leading: ReorderableDragStartListener(
                      index: i,
                      child: const Icon(Icons.drag_indicator,
                          color: BrandColors.slateGrey),
                    ),
                    title: Row(
                      children: [
                        GestureDetector(
                          onTap: () => editStop(i),
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
                      onPressed:
                          colors.length <= 1 ? null : () => removeStop(i),
                    ),
                  );
                },
              ),
            ),
            if (colors.length < 5)
              TextButton(
                onPressed: addStop,
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
  required String lampId,
}) {
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    backgroundColor: BrandColors.midnightBlack,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) => FractionallySizedBox(
      heightFactor: 0.6,
      child: BaseEditorSheet(lampId: lampId),
    ),
  );
}
