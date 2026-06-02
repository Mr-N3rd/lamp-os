import 'dart:async';

import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../../core/routing/routes.dart';
import '../../../../core/theme/brand_colors.dart';
import '../../../../core/widgets/lamp_icon.dart';
import '../../../../core/widgets/status_dot.dart';
import '../../../control/application/control_notifier.dart';
import '../../../nearby/application/lamp_route_resolver.dart';
import '../../../inventory/application/active_lamp_notifier.dart';
import '../../../inventory/application/inventory_notifier.dart';
import '../../../inventory/domain/inventory_lamp.dart';
import '../../../inventory/domain/lamp_colors.dart';
import '../../../lamp_shell/application/lamp_status.dart';
import '../../../nearby/application/nearby_lamps_notifier.dart';
import '../../../nearby/domain/nearby_lamp.dart';
import '../../domain/last_seen.dart';

Future<void> showLampPickerSheet(
  BuildContext context, {
  required String currentLampId,
}) {
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    backgroundColor: BrandColors.midnightBlack,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) => FractionallySizedBox(
      heightFactor: 0.75,
      child: LampPickerSheet(currentLampId: currentLampId),
    ),
  );
}

class LampPickerSheet extends ConsumerWidget {
  const LampPickerSheet({super.key, required this.currentLampId});

  final String currentLampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final inventory =
        ref.watch(inventoryNotifierProvider).value ?? const [];
    // Nearby is still read for status-dot decoration on owned-lamp rows
    // (each row needs to know if its lamp is in range / on mesh / BT-only).
    // The "Other nearby lamps" section was removed — discovery of unowned
    // lamps belongs to the Add Lamp flow, not the switch picker.
    final nearby = ref.watch(nearbyLampsNotifierProvider);

    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              children: [
                const Text(
                  'Your lamps',
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
            const SizedBox(height: 8),
            Expanded(
              child: ListView(
                children: [
                  for (final l in inventory)
                    _InventoryLampTile(
                      lamp: l,
                      isCurrent: l.id == currentLampId,
                      nearby: nearby,
                    ),
                ],
              ),
            ),
            const Divider(color: BrandColors.slateGrey, height: 1),
            const SizedBox(height: 4),
            TextButton.icon(
              icon: const Icon(Icons.add, size: 18),
              label: const Text('Add a lamp'),
              onPressed: () {
                Navigator.pop(context);
                // `push` not `go` — user must be able to abort the add-lamp
                // flow and return to the lamp shell. `go` would replace
                // history and trap them in the wizard.
                GoRouter.maybeOf(context)?.push(AppRoutes.addLamp);
              },
            ),
          ],
        ),
      ),
    );
  }
}

class _InventoryLampTile extends ConsumerWidget {
  const _InventoryLampTile({
    required this.lamp,
    required this.isCurrent,
    required this.nearby,
  });

  final InventoryLamp lamp;
  final bool isCurrent;
  final List<NearbyLamp> nearby;

  Future<void> _onTap(BuildContext context, WidgetRef ref) async {
    if (isCurrent) {
      Navigator.pop(context);
      return;
    }
    await ref.read(activeLampNotifierProvider.notifier).set(lamp.id);
    if (context.mounted) {
      Navigator.pop(context);
      // Only navigate if we're inside a GoRouter tree. Tests that pump the
      // sheet directly (without GoRouter) skip the route change — the
      // active-lamp state change is what matters in that scenario.
      // routeForLamp sends BT-only firmware to the dedicated info pane
      // instead of trapping the user on the ConnectingView.
      GoRouter.maybeOf(context)?.go(routeForLamp(lamp.id, nearby));
    }
  }

  Future<void> _confirmRemove(BuildContext context, WidgetRef ref) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: BrandColors.midnightBlack,
        title: const Text('Remove this lamp?',
            style: TextStyle(color: BrandColors.lampWhite)),
        content: Text(
          '${lamp.name} will be removed from your lamps on this phone. '
          "The lamp itself keeps its name, password, and Wi-Fi — you can "
          'add it back later from the picker.',
          style: const TextStyle(color: BrandColors.fogGrey),
        ),
        actions: [
          TextButton(
              onPressed: () => Navigator.of(ctx).pop(false),
              child: const Text('Cancel')),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: BrandColors.error),
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('Remove'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    final activeBefore = ref.read(activeLampNotifierProvider).value;
    await ref.read(inventoryNotifierProvider.notifier).remove(lamp.id);
    ref.invalidate(controlNotifierProvider(lamp.id));
    if (lamp.id == activeBefore) {
      final remaining = ref.read(inventoryNotifierProvider).value ?? const [];
      if (remaining.isEmpty) {
        await ref.read(activeLampNotifierProvider.notifier).clear();
      } else {
        await ref.read(activeLampNotifierProvider.notifier).set(remaining.first.id);
      }
    }
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // The currently-active lamp's row shows mesh when we're connected to
    // it; every other row's connected flag is false from this screen's
    // perspective (we only hold one BLE connection at a time).
    final connected = isCurrent &&
        (ref.watch(controlNotifierProvider(lamp.id)).value?.connected ??
            false);
    final status = statusFor(
      lampId: lamp.id,
      nearby: nearby,
      connected: connected,
    );
    final isOffline = status == StatusKind.offline;
    final colors = resolveLampColors(
      inv: lamp,
      near: nearby.firstWhereOrNull((n) => n.id == lamp.id),
    );
    return InkWell(
      borderRadius: BorderRadius.circular(12),
      onTap: (isOffline && !isCurrent) ? null : () => _onTap(context, ref),
      onLongPress: () => _confirmRemove(context, ref),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 10),
        child: Row(
          children: [
            StatusDot(kind: status, size: 14),
            const SizedBox(width: 12),
            LampIcon(
              shade: colors.shade,
              base: colors.base,
              size: 36,
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                lamp.name,
                style: const TextStyle(
                  color: BrandColors.lampWhite,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
            if (isCurrent)
              Container(
                padding: const EdgeInsets.symmetric(
                  horizontal: 8,
                  vertical: 4,
                ),
                decoration: BoxDecoration(
                  borderRadius: BorderRadius.circular(999),
                  color: BrandColors.lumenGreen.withValues(alpha: 0.18),
                ),
                child: const Text(
                  'active',
                  style: TextStyle(
                    fontSize: 10,
                    color: BrandColors.lumenGreen,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              )
            else if (isOffline)
              Text(
                lamp.lastSeenEpochMs == null
                    ? 'Not seen yet'
                    : formatLastSeen(lamp.lastSeenEpochMs!, DateTime.now()),
                style: const TextStyle(color: BrandColors.slateGrey, fontSize: 12),
              )
            else
              const Icon(Icons.chevron_right, color: BrandColors.slateGrey),
          ],
        ),
      ),
    );
  }
}

