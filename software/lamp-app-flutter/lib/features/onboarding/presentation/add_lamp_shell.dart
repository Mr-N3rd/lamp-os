import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../application/add_lamp_notifier.dart';
import '../domain/add_lamp_state.dart';
import 'widgets/add_lamp_done_step.dart';
import 'widgets/add_lamp_name_step.dart';
import 'widgets/add_lamp_password_step.dart';
import 'widgets/add_lamp_scan_step.dart';

class AddLampShell extends ConsumerWidget {
  const AddLampShell({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final step = ref.watch(addLampNotifierProvider).step;
    final body = switch (step) {
      AddLampStep.scan => const AddLampScanStep(),
      AddLampStep.name => const AddLampNameStep(),
      AddLampStep.password => const AddLampPasswordStep(),
      AddLampStep.done => const AddLampDoneStep(),
    };
    return Scaffold(
      appBar: AppBar(
        title: const Text('Add a lamp'),
        bottom: PreferredSize(
          preferredSize: const Size.fromHeight(8),
          child: _ProgressDots(currentIndex: step.index),
        ),
      ),
      body: body,
    );
  }
}

class _ProgressDots extends StatelessWidget {
  const _ProgressDots({required this.currentIndex});
  final int currentIndex;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: List.generate(4, (i) {
          final active = i == currentIndex;
          return Container(
            margin: const EdgeInsets.symmetric(horizontal: 4),
            width: active ? 24 : 8,
            height: 8,
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(999),
              color: active
                  ? BrandColors.glowPink
                  : BrandColors.slateGrey.withValues(alpha: 0.5),
            ),
          );
        }),
      ),
    );
  }
}
