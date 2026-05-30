import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';

class OnboardingPlaceholder extends StatelessWidget {
  const OnboardingPlaceholder({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Text(
              'No lamps yet',
              style: TextStyle(
                color: BrandColors.lampWhite,
                fontWeight: FontWeight.w600,
                fontSize: 18,
              ),
            ),
            const SizedBox(height: 12),
            const Text(
              'Tap below to set up your first lamp.',
              style: TextStyle(color: BrandColors.fogGrey),
            ),
            const SizedBox(height: 24),
            FilledButton.icon(
              // `push` so the add-lamp wizard's back/cancel returns here.
              onPressed: () => context.push('/onboarding/add'),
              icon: const Icon(Icons.add),
              label: const Text('Add a lamp'),
            ),
            const SizedBox(height: 12),
            TextButton(
              // `push` so the debug scan screen's back arrow returns here.
              onPressed: () => context.push('/devices'),
              child: const Text('Live scan (debug)'),
            ),
          ],
        ),
      ),
    );
  }
}
