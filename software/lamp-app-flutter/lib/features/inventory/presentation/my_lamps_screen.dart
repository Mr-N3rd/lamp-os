import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/lamp_icon.dart';
import '../../../core/widgets/status_dot.dart';
import '../../control/application/control_notifier.dart';
import '../../inventory/application/active_lamp_notifier.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../../inventory/domain/inventory_lamp.dart';
import '../../inventory/domain/lamp_colors.dart';
import '../../lamp_shell/application/lamp_status.dart';
import '../../nearby/application/lamp_route_resolver.dart';
import '../../nearby/application/nearby_lamps_notifier.dart';
import '../../nearby/application/scan_grace_provider.dart';
import '../../nearby/domain/nearby_lamp.dart';
import '../domain/last_seen.dart';

/// The app's home / landing screen for users with at least one lamp in
/// their inventory. Lists every owned lamp with status + last-seen colors,
/// plus an "Adopt a lamp" tile at the end. Tapping a lamp routes to its
/// control screen — or, when the lamp is currently visible on BLE but its
/// firmware doesn't speak the mesh protocol (`isMesh == false`), to the
/// dedicated BT-only screen.
///
/// Replaced the previous "auto-redirect to the last-active lamp on app
/// open" flow so users can see their full inventory at a glance and pick
/// which lamp they want. "Other nearby lamps" (unowned, freshly-
/// discovered devices) belong to the Add Lamp flow now, not this screen.
class MyLampsScreen extends ConsumerWidget {
  const MyLampsScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final inventory =
        ref.watch(inventoryNotifierProvider).value ?? const [];
    final nearby = ref.watch(nearbyLampsNotifierProvider);
    final activeId = ref.watch(activeLampNotifierProvider).value;
    // Materialise the id→NearbyLamp map once at the screen level so the
    // per-tile _nearbyHit() lookup is O(1) instead of an O(n) linear
    // scan. At an event with 22 lamps, the old loop was running ~484
    // comparisons per build cycle (per tile × per nearby entry); the
    // map cuts that to a hash lookup (audit perf-H6).
    final nearbyById = <String, NearbyLamp>{for (final n in nearby) n.id: n};

    return Scaffold(
      appBar: AppBar(
        title: const Text('My lamps'),
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
          children: [
            for (final l in inventory)
              _MyLampTile(
                lamp: l,
                nearby: nearby,
                nearbyById: nearbyById,
                isCurrent: l.id == activeId,
              ),
            const SizedBox(height: 8),
            const Divider(color: BrandColors.slateGrey, height: 1),
            const SizedBox(height: 8),
            _AddLampTile(),
          ],
        ),
      ),
    );
  }
}

class _MyLampTile extends ConsumerWidget {
  const _MyLampTile({
    required this.lamp,
    required this.nearby,
    required this.nearbyById,
    required this.isCurrent,
  });

  final InventoryLamp lamp;
  final List<NearbyLamp> nearby;

  /// Pre-built id→NearbyLamp index so the per-tile hit check is O(1)
  /// instead of O(n). Same list as `nearby` but materialised at the
  /// screen level (audit perf-H6).
  final Map<String, NearbyLamp> nearbyById;

  /// True iff this tile represents the currently-active lamp. Used to
  /// guard the `controlNotifierProvider` watch — watching every tile's
  /// notifier would fan-out N parallel BLE connects on screen mount and
  /// jam the GATT queue, making the lamp the user actually tapped slow
  /// to come online. Only the active lamp's link is held; other tiles
  /// fall back to nearby-scan-derived status.
  final bool isCurrent;

  NearbyLamp? _nearbyHit() => nearbyById[lamp.id];

  void _onTap(BuildContext context, WidgetRef ref) {
    // Park the active-lamp pointer so AppBar pickers / future opens
    // remember which lamp we're focused on.
    unawaited(
        ref.read(activeLampNotifierProvider.notifier).set(lamp.id));
    // routeForLamp picks BT-only vs. control based on the lamp's
    // mesh-protocol advertisement bit, falling back to the inventory's
    // cached `lastKnownIsMesh` for offline lamps. See its docstring.
    final inv = ref.read(inventoryNotifierProvider).value;
    GoRouter.maybeOf(context)?.push(
      routeForLamp(lamp.id, nearby, inventory: inv),
    );
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // Only the active lamp's controlNotifier is materialised here. For
    // every other tile we pass `connected: false` and let `statusFor`
    // derive the dot from the nearby-scan stream (mesh-via-adv vs
    // bluetooth vs offline). See class doc on `isCurrent` above.
    //
    // `.select()` slices the watch to JUST the `connected` boolean —
    // without it, every color-slider tick on the active lamp would
    // rebuild every tile in the list (each one re-evaluates this
    // expression on any state change). With the slice, non-active
    // tiles' watch result stays `false` and they don't rebuild.
    final connected = isCurrent &&
        ref.watch(controlNotifierProvider(lamp.id).select(
          (async) => async.value?.connected ?? false,
        ));
    final inScanGrace = ref.watch(scanGraceActiveProvider);
    final status = statusForById(
      lampId: lamp.id,
      nearbyById: nearbyById,
      connected: connected,
      inScanGrace: inScanGrace,
    );
    return InkWell(
      borderRadius: BorderRadius.circular(12),
      onTap: () => _onTap(context, ref),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 12),
        child: Row(
          children: [
            StatusDot(kind: status, size: 14),
            const SizedBox(width: 12),
            Builder(builder: (_) {
              final colors = resolveLampColors(inv: lamp, near: _nearbyHit());
              return LampIcon(
                shade: colors.shade,
                base: colors.base,
                size: 44,
              );
            }),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    lamp.name,
                    style: const TextStyle(
                      color: BrandColors.lampWhite,
                      fontSize: 15,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(
                    _subtitle(status, lamp),
                    style: const TextStyle(
                      color: BrandColors.slateGrey,
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

  String _subtitle(StatusKind status, InventoryLamp lamp) {
    switch (status) {
      case StatusKind.mesh:
        return 'In range';
      case StatusKind.bluetooth:
        return 'Bluetooth only';
      case StatusKind.searching:
        return 'Searching…';
      case StatusKind.offline:
        if (lamp.lastSeenEpochMs == null) return 'Not seen yet';
        return formatLastSeen(lamp.lastSeenEpochMs!, DateTime.now());
    }
  }
}

/// Catch-all "Adopt a lamp" tile rendered after the inventory list.
class _AddLampTile extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return InkWell(
      borderRadius: BorderRadius.circular(12),
      onTap: () => GoRouter.maybeOf(context)?.push(AppRoutes.addLamp),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 14),
        child: Row(
          children: [
            Container(
              width: 44,
              height: 44,
              decoration: BoxDecoration(
                color: BrandColors.slateGrey.withValues(alpha: 0.15),
                borderRadius: BorderRadius.circular(8),
              ),
              child: const Icon(Icons.add, color: BrandColors.lampWhite),
            ),
            const SizedBox(width: 14),
            const Expanded(
              child: Text(
                'Adopt a lamp',
                style: TextStyle(
                  color: BrandColors.lampWhite,
                  fontSize: 15,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
            const Icon(Icons.chevron_right, color: BrandColors.slateGrey),
          ],
        ),
      ),
    );
  }
}

