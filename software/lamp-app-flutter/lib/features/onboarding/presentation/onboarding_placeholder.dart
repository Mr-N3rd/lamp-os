import 'package:flutter/material.dart';

import '../../../core/theme/brand_colors.dart';

class OnboardingPlaceholder extends StatelessWidget {
  const OnboardingPlaceholder({super.key});

  @override
  Widget build(BuildContext context) {
    return const Scaffold(
      body: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              'No lamps yet',
              style: TextStyle(
                color: BrandColors.lampWhite,
                fontWeight: FontWeight.w600,
                fontSize: 18,
              ),
            ),
            SizedBox(height: 12),
            Text(
              '+ Add a lamp (coming in Phase 1)',
              style: TextStyle(color: BrandColors.fogGrey),
            ),
          ],
        ),
      ),
    );
  }
}
