import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../control/application/control_notifier.dart';
import '../../control/domain/sections.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../domain/expression_meta.dart';

/// Entry point for adding a new expression. Replaces the previous "open the
/// editor with a blank draft" flow: now the user picks Target first
/// (Shade / Base / Both), then picks one of the expression types from a
/// list of friendly cards. Combos already in use are disabled in-place.
class AddExpressionPickerScreen extends ConsumerStatefulWidget {
  const AddExpressionPickerScreen({super.key, required this.lampId});

  final String lampId;

  @override
  ConsumerState<AddExpressionPickerScreen> createState() =>
      _AddExpressionPickerScreenState();
}

class _AddExpressionPickerScreenState
    extends ConsumerState<AddExpressionPickerScreen> {
  int _target = 3; // TARGET_BOTH default

  @override
  Widget build(BuildContext context) {
    final async = ref.watch(controlNotifierProvider(widget.lampId));
    return Scaffold(
      appBar: AppBar(
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () {
            final router = GoRouter.maybeOf(context);
            if (router != null && router.canPop()) {
              router.pop();
            } else {
              Navigator.maybeOf(context)?.maybePop();
            }
          },
          tooltip: 'Back',
        ),
        title: const Text('New expression'),
      ),
      body: async.when(
        loading: () => ConnectingView(deviceId: widget.lampId),
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
        data: (state) => _Body(
          lampId: widget.lampId,
          target: _target,
          onTargetChanged: (t) => setState(() => _target = t),
          existing: state.expressions.expressions,
        ),
      ),
    );
  }
}

class _Body extends StatelessWidget {
  const _Body({
    required this.lampId,
    required this.target,
    required this.onTargetChanged,
    required this.existing,
  });

  final String lampId;
  final int target;
  final ValueChanged<int> onTargetChanged;
  final List<ExpressionConfig> existing;

  bool _isTaken(String type, int t) =>
      existing.any((e) => e.type == type && e.target == t);

  bool _targetFull(int t) {
    // Disable a target only when every expression type already uses it.
    return ExpressionTypeMeta.all.every((m) => _isTaken(m.key, t));
  }

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
      children: [
        const Padding(
          padding: EdgeInsets.only(top: 8, bottom: 8),
          child: Text(
            'Where should this expression play?',
            style: TextStyle(color: BrandColors.lampWhite, fontSize: 15),
          ),
        ),
        Center(
          child: SegmentedButton<int>(
            showSelectedIcon: false,
            segments: <ButtonSegment<int>>[
              ButtonSegment(
                value: 1,
                label: const Text('Shade'),
                enabled: !_targetFull(1),
              ),
              ButtonSegment(
                value: 2,
                label: const Text('Base'),
                enabled: !_targetFull(2),
              ),
              ButtonSegment(
                value: 3,
                label: const Text('Both'),
                enabled: !_targetFull(3),
              ),
            ],
            selected: {target},
            onSelectionChanged: (s) => onTargetChanged(s.first),
          ),
        ),
        const SizedBox(height: 24),
        const Padding(
          padding: EdgeInsets.only(bottom: 8, left: 4),
          child: Text(
            'Pick an expression',
            style: TextStyle(color: BrandColors.lampWhite, fontSize: 15),
          ),
        ),
        for (final meta in ExpressionTypeMeta.all)
          _ExpressionCard(
            meta: meta,
            taken: _isTaken(meta.key, target),
            onTap: () {
              GoRouter.maybeOf(context)?.push(
                AppRoutes.expressionEditor(lampId, meta.key, target),
              );
            },
          ),
      ],
    );
  }
}

class _ExpressionCard extends StatelessWidget {
  const _ExpressionCard({
    required this.meta,
    required this.taken,
    required this.onTap,
  });

  final ExpressionTypeMeta meta;
  final bool taken;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final disabledOpacity = taken ? 0.35 : 1.0;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Material(
        color: BrandColors.lampWhite.withValues(alpha: 0.04),
        borderRadius: BorderRadius.circular(12),
        child: InkWell(
          borderRadius: BorderRadius.circular(12),
          onTap: taken ? null : onTap,
          child: Container(
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              borderRadius: BorderRadius.circular(12),
              border: Border.all(
                color: BrandColors.lampWhite.withValues(alpha: 0.06),
              ),
            ),
            child: Opacity(
              opacity: disabledOpacity,
              child: Row(
                children: [
                  Container(
                    width: 44,
                    height: 44,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: BrandColors.auroraBlue.withValues(alpha: 0.18),
                    ),
                    child: Icon(
                      meta.icon,
                      color: BrandColors.auroraBlue,
                    ),
                  ),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(
                          children: [
                            Text(
                              meta.name,
                              style: const TextStyle(
                                color: BrandColors.lampWhite,
                                fontSize: 16,
                                fontWeight: FontWeight.w600,
                              ),
                            ),
                            if (taken) ...[
                              const SizedBox(width: 8),
                              Container(
                                padding: const EdgeInsets.symmetric(
                                    horizontal: 6, vertical: 2),
                                decoration: BoxDecoration(
                                  borderRadius: BorderRadius.circular(999),
                                  color: BrandColors.slateGrey
                                      .withValues(alpha: 0.2),
                                ),
                                child: const Text(
                                  'in use',
                                  style: TextStyle(
                                    fontSize: 10,
                                    color: BrandColors.slateGrey,
                                    fontWeight: FontWeight.w600,
                                  ),
                                ),
                              ),
                            ],
                          ],
                        ),
                        const SizedBox(height: 2),
                        Text(
                          meta.tagline,
                          style: const TextStyle(
                            color: BrandColors.fogGrey,
                            fontSize: 12,
                          ),
                        ),
                      ],
                    ),
                  ),
                  if (!taken)
                    const Icon(Icons.chevron_right,
                        color: BrandColors.slateGrey),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
