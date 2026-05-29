// lib/features/onboarding/presentation/widgets/add_lamp_done_step.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../../core/routing/routes.dart';
import '../../../../core/theme/brand_colors.dart';
import '../../application/add_lamp_notifier.dart';

class AddLampDoneStep extends ConsumerWidget {
  const AddLampDoneStep({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final state = ref.watch(addLampNotifierProvider);
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.center,
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const Icon(
            Icons.check_circle,
            color: BrandColors.lumenGreen,
            size: 64,
          ),
          const SizedBox(height: 16),
          Text(
            '${state.name} is ready',
            style: const TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 22,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            'Your lamp is connected and added to your collection.',
            style: TextStyle(color: BrandColors.fogGrey),
            textAlign: TextAlign.center,
          ),
          const SizedBox(height: 32),
          FilledButton(
            onPressed: () {
              ref.read(addLampNotifierProvider.notifier).reset();
              context.go(AppRoutes.control(state.deviceId));
            },
            child: const Text('Open your lamp'),
          ),
        ],
      ),
    );
  }
}
