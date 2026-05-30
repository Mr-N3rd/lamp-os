import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../application/add_lamp_notifier.dart';
import '../domain/add_lamp_state.dart';
import 'widgets/add_lamp_done_step.dart';
import 'widgets/add_lamp_name_step.dart';
import 'widgets/add_lamp_password_step.dart';
import 'widgets/add_lamp_scan_step.dart';

class AddLampShell extends ConsumerStatefulWidget {
  const AddLampShell({super.key});

  @override
  ConsumerState<AddLampShell> createState() => _AddLampShellState();
}

class _AddLampShellState extends ConsumerState<AddLampShell> {
  /// Cached at initState so `dispose` can call into the notifier without
  /// touching `ref` after the widget is deactivated (Riverpod 4 blocks
  /// `ref` use in `dispose`).
  late final AddLampNotifier _notifier;

  @override
  void initState() {
    super.initState();
    _notifier = ref.read(addLampNotifierProvider.notifier);
  }

  @override
  void dispose() {
    // Reset the wizard to its initial step on every exit path — back
    // arrow, GoRouter pop, system back. Otherwise `addLampNotifier` is
    // `keepAlive: true` and the user's previous `done` state persists,
    // so the next time they open `/onboarding/add` they land directly
    // on the success screen with no scan list. The Done step's own
    // "Open your lamp" handler also calls reset(); idempotent.
    //
    // Run via `Future.microtask` so the `state = …` write doesn't fire
    // during widget-tree teardown — Riverpod 4 blocks mutations in that
    // window (`_debugCanModifyProviders`). The microtask runs on the
    // very next event-loop turn, after the current frame finalizes.
    //
    // The try/catch is for tests: when the `ProviderContainer` is torn
    // down before the microtask runs, the notifier is already disposed
    // and `state =` throws `UnmountedRefException`. In production the
    // provider is `keepAlive: true`, so the reset always lands safely.
    Future.microtask(() {
      try {
        _notifier.reset();
      } catch (_) {
        // Container disposed before microtask ran — nothing to reset.
      }
    });
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
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
