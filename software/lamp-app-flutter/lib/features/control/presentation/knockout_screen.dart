import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../application/control_notifier.dart';
import 'widgets/connecting_view.dart';

class KnockoutScreen extends ConsumerWidget {
  const KnockoutScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final async = ref.watch(controlNotifierProvider(lampId));
    final inv = ref.watch(inventoryNotifierProvider).value;
    final name = inv
            ?.firstWhereOrNull((l) => l.id == lampId)
            ?.name ??
        lampId;

    return Scaffold(
      appBar: AppBar(
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () => context.pop(),
          tooltip: 'Back',
        ),
        title: Text('Pixel Knockout · $name'),
      ),
      body: async.when(
        loading: () => ConnectingView(deviceId: lampId),
        error: (e, _) => Center(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Text(
              'Could not reach this lamp: $e',
              style: const TextStyle(color: BrandColors.fogGrey),
              textAlign: TextAlign.center,
            ),
          ),
        ),
        data: (state) {
          final notifier =
              ref.read(controlNotifierProvider(lampId).notifier);
          final pixelCount = state.base.px;
          return Column(
            children: [
              Expanded(
                child: ListView.builder(
                  itemCount: pixelCount,
                  itemBuilder: (ctx, i) {
                    final brightness = state.base.knockout[i] ?? 100;
                    return _KnockoutRow(
                      index: i,
                      brightness: brightness,
                      onChanged: (v) => notifier.setKnockoutPixel(i, v),
                    );
                  },
                ),
              ),
              SafeArea(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                  child: Row(
                    children: [
                      Text(
                        '${state.base.knockout.length} edited',
                        style: const TextStyle(
                          color: BrandColors.fogGrey,
                          fontSize: 12,
                        ),
                      ),
                      const Spacer(),
                      TextButton(
                        onPressed: () {
                          for (var i = 0; i < pixelCount; i++) {
                            notifier.setKnockoutPixel(i, 100);
                          }
                        },
                        child: const Text('Reset all'),
                      ),
                    ],
                  ),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

class _KnockoutRow extends StatelessWidget {
  const _KnockoutRow({
    required this.index,
    required this.brightness,
    required this.onChanged,
  });

  final int index;
  final int brightness;
  final ValueChanged<int> onChanged;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
      child: Row(
        children: [
          SizedBox(
            width: 32,
            child: Text(
              '#$index',
              style: const TextStyle(
                color: BrandColors.slateGrey,
                fontSize: 11,
                fontFamily: 'monospace',
              ),
            ),
          ),
          Expanded(
            child: Slider(
              min: 0,
              max: 100,
              divisions: 100,
              value: brightness.toDouble(),
              onChanged: (v) => onChanged(v.round()),
            ),
          ),
          SizedBox(
            width: 40,
            child: Text(
              '$brightness%',
              textAlign: TextAlign.right,
              style: const TextStyle(
                color: BrandColors.fogGrey,
                fontSize: 11,
                fontFamily: 'monospace',
              ),
            ),
          ),
        ],
      ),
    );
  }
}
