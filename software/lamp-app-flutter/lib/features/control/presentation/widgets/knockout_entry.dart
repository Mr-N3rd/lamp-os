import 'package:flutter/material.dart';
import 'package:go_router/go_router.dart';

import '../../../../core/routing/routes.dart';
import '../../../../core/theme/brand_colors.dart';

class KnockoutEntry extends StatelessWidget {
  const KnockoutEntry({
    super.key,
    required this.lampId,
    required this.editedCount,
  });

  final String lampId;
  final int editedCount;

  @override
  Widget build(BuildContext context) {
    return InkWell(
      // `push` not `go` — KnockoutScreen has an explicit back button that
      // calls `context.pop()`; `go` would replace history and strand it.
      onTap: () => context.push(AppRoutes.knockout(lampId)),
      borderRadius: BorderRadius.circular(12),
      child: Container(
        margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
        padding: const EdgeInsets.all(16),
        decoration: BoxDecoration(
          color: Colors.white.withValues(alpha: 0.04),
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
        ),
        child: Row(
          children: [
            const Icon(Icons.grain, color: BrandColors.slateGrey, size: 20),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text(
                    'Knockout',
                    style: TextStyle(
                      color: BrandColors.lampWhite,
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                      letterSpacing: 0.4,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    editedCount == 0
                        ? 'No pixels dimmed'
                        : '$editedCount pixel${editedCount == 1 ? '' : 's'} dimmed',
                    style: const TextStyle(
                      color: BrandColors.fogGrey,
                      fontSize: 12,
                    ),
                  ),
                ],
              ),
            ),
            const Icon(Icons.chevron_right, color: BrandColors.slateGrey),
          ],
        ),
      ),
    );
  }
}
