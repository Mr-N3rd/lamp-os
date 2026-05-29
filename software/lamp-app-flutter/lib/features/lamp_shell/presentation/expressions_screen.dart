import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../control/application/control_notifier.dart';
import '../../control/domain/sections.dart';
import '../../control/presentation/widgets/connecting_view.dart';

class ExpressionsScreen extends ConsumerWidget {
  const ExpressionsScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final async = ref.watch(controlNotifierProvider(lampId));
    return Scaffold(
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
          if (state.expressions.expressions.isEmpty) {
            return const Center(
              child: Padding(
                padding: EdgeInsets.all(24),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      'No expressions yet',
                      style: TextStyle(
                        color: BrandColors.lampWhite,
                        fontSize: 16,
                      ),
                    ),
                    SizedBox(height: 8),
                    Text(
                      'Tap + to add a Glitch, Pulse, Breath or Shift effect.',
                      style: TextStyle(color: BrandColors.fogGrey, fontSize: 12),
                      textAlign: TextAlign.center,
                    ),
                  ],
                ),
              ),
            );
          }
          return ListView.builder(
            padding: const EdgeInsets.symmetric(vertical: 8),
            itemCount: state.expressions.expressions.length,
            itemBuilder: (ctx, i) {
              final e = state.expressions.expressions[i];
              return _ExpressionTile(
                lampId: lampId,
                expression: e,
                onToggle: (v) async {
                  await notifier.upsertExpression(ExpressionConfig(
                    type: e.type,
                    enabled: v,
                    colors: e.colors,
                    intervalMin: e.intervalMin,
                    intervalMax: e.intervalMax,
                    target: e.target,
                    parameters: e.parameters,
                  ));
                },
                onConfirmDelete: () =>
                    _confirmDelete(context, e.type),
                onDelete: () async {
                  await notifier.removeExpression(
                    type: e.type,
                    target: e.target,
                  );
                  if (!context.mounted) return;
                  ScaffoldMessenger.of(context).showSnackBar(
                    SnackBar(
                      content: Text('Removed "${e.type}"'),
                      duration: const Duration(seconds: 4),
                      action: SnackBarAction(
                        label: 'UNDO',
                        onPressed: () =>
                            notifier.upsertExpression(e),
                      ),
                    ),
                  );
                },
              );
            },
          );
        },
      ),
      floatingActionButton: FloatingActionButton(
        onPressed: () => GoRouter.maybeOf(context)?.go(
          AppRoutes.expressionEditor(lampId, '_new', 3),
        ),
        tooltip: 'Add expression',
        child: const Icon(Icons.add),
      ),
    );
  }
}

/// Shows the confirmation dialog for deleting an expression. Returns `true`
/// if the user confirmed the delete.
Future<bool> _confirmDelete(BuildContext context, String type) async {
  final result = await showDialog<bool>(
    context: context,
    builder: (ctx) => AlertDialog(
      title: Text(type.isEmpty
          ? 'Delete this expression?'
          : 'Delete the "$type" expression?'),
      content: const Text(
        'You can undo this from the snackbar that appears after deleting.',
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(ctx, false),
          child: const Text('Cancel'),
        ),
        FilledButton(
          style: FilledButton.styleFrom(backgroundColor: Colors.redAccent),
          onPressed: () => Navigator.pop(ctx, true),
          child: const Text('Delete'),
        ),
      ],
    ),
  );
  return result ?? false;
}

class _ExpressionTile extends StatelessWidget {
  const _ExpressionTile({
    required this.lampId,
    required this.expression,
    required this.onToggle,
    required this.onConfirmDelete,
    required this.onDelete,
  });

  final String lampId;
  final ExpressionConfig expression;
  final ValueChanged<bool> onToggle;
  final Future<bool> Function() onConfirmDelete;
  final Future<void> Function() onDelete;

  String get _targetLabel => switch (expression.target) {
        1 => 'shade',
        2 => 'base',
        3 => 'both',
        _ => 'unknown',
      };

  @override
  Widget build(BuildContext context) {
    return Dismissible(
      key: ValueKey('${expression.type}-${expression.target}'),
      direction: DismissDirection.endToStart,
      background: Container(
        alignment: Alignment.centerRight,
        padding: const EdgeInsets.only(right: 24),
        color: Colors.redAccent.withValues(alpha: 0.3),
        child: const Icon(Icons.delete, color: Colors.redAccent),
      ),
      confirmDismiss: (_) => onConfirmDelete(),
      onDismissed: (_) => onDelete(),
      child: InkWell(
        onTap: () => GoRouter.maybeOf(context)?.go(
          AppRoutes.expressionEditor(
              lampId, expression.type, expression.target),
        ),
        child: Container(
          margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
          padding: const EdgeInsets.all(16),
          decoration: BoxDecoration(
            color: Colors.white.withValues(alpha: 0.04),
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
          ),
          child: Row(
            children: [
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      expression.type.isEmpty ? '(unnamed)' : expression.type,
                      style: const TextStyle(
                        color: BrandColors.lampWhite,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(height: 4),
                    Text(
                      '$_targetLabel · '
                      '${expression.intervalMin}-${expression.intervalMax}s',
                      style: const TextStyle(
                        color: BrandColors.fogGrey,
                        fontSize: 12,
                      ),
                    ),
                  ],
                ),
              ),
              Switch(value: expression.enabled, onChanged: onToggle),
            ],
          ),
        ),
      ),
    );
  }
}
