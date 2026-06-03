import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_svg/flutter_svg.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/utils/tap_counter.dart';
import '../../../core/widgets/info_panel.dart';
import '../../../core/widgets/lamp_icon.dart';
import '../../../core/widgets/status_dot.dart';
import '../../control/application/advanced_session.dart';
import '../../control/application/control_notifier.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../../inventory/domain/inventory_lamp.dart';
import '../../inventory/domain/lamp_colors.dart';
import '../../inventory/domain/last_seen.dart';
import '../../nearby/application/nearby_lamps_notifier.dart';
import '../../nearby/application/seen_lamps_notifier.dart';
import '../../nearby/domain/nearby_lamp.dart';
import '../../nearby/domain/seen_lamp.dart';
import '../application/lamp_status.dart';

/// Info tab — mirror of the Vue app's Info.vue:
///   - Lamplit text-logo (tap 5× in 3s to unlock advanced settings).
///   - Marketing blurb / society link.
///   - Nearby Lamps list (BLE + future-grid).
class InfoScreen extends ConsumerStatefulWidget {
  const InfoScreen({super.key, required this.lampId});
  final String lampId;

  @override
  ConsumerState<InfoScreen> createState() => _InfoScreenState();
}

class _InfoScreenState extends ConsumerState<InfoScreen> {
  late final TapCounter _tap;

  @override
  void initState() {
    super.initState();
    _tap = TapCounter(
      count: 5,
      window: const Duration(seconds: 3),
      onTriggered: () {
        // Session-only unlock: flips the app-side advancedSession flag for
        // this lamp. Resets to false on BLE disconnect (handled by
        // ControlNotifier._onConnectionChange). The user re-does the tap
        // gesture each session — by design, advanced mode never lingers
        // across reconnects.
        ref.read(advancedSessionProvider(widget.lampId).notifier).enable();
        if (context.mounted) {
          // Land on the Setup hub rather than drilling straight into the
          // Advanced LED screen — that way the user sees the lamp name +
          // home-mode rows alongside the now-unlocked Advanced LED row,
          // matching the old mobile app's behavior.
          GoRouter.maybeOf(context)?.push(AppRoutes.setup(widget.lampId));
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
                duration: Duration(seconds: 2),
                content: Text('Advanced settings unlocked')),
          );
        }
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final nearby = ref.watch(nearbyLampsNotifierProvider);
    final seen = ref.watch(seenLampsNotifierProvider);
    final connected = ref.watch(controlNotifierProvider(widget.lampId)
        .select((async) => async.value?.connected ?? false));
    // Owned lamps in inventory carry cached "last-seen" colors written
    // by controlNotifier._updateSeen on every connect. We prefer those
    // over the live BLE advertisement bytes for the nearby tiles below:
    // v2-firmware lamps drop shade from the adv to fit NimBLE's length
    // cap (see ble_scanner.dart), so reading `shadeRgb` directly would
    // render black for them.
    final inventory =
        ref.watch(inventoryNotifierProvider).value ?? const [];
    final inventoryById = {for (final l in inventory) l.id: l};
    // Seen-but-not-currently-nearby: filter so the Seen column doesn't
    // duplicate rows that are already showing live in the Nearby column.
    final nearbyIds = {for (final l in nearby) l.id};
    final seenOnly = [
      for (final l in seen)
        if (!nearbyIds.contains(l.id)) l,
    ];
    final now = DateTime.now();
    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
      children: [
        const SizedBox(height: 24),
        // Tap target for the secret unlock — keep it generous.
        GestureDetector(
          onTap: _tap.record,
          behavior: HitTestBehavior.opaque,
          child: const Center(
            child: Padding(
              padding: EdgeInsets.symmetric(vertical: 16),
              child: _LamplitWordmark(),
            ),
          ),
        ),
        const SizedBox(height: 8),
        const Center(
          child: Text(
            '✦  Sparking inspiration through shared creative experiences',
            textAlign: TextAlign.center,
            style: TextStyle(color: BrandColors.fogGrey, fontSize: 12),
          ),
        ),
        const SizedBox(height: 16),
        const InfoPanel(
          child: Text.rich(
            TextSpan(
              children: [
                TextSpan(
                    text:
                        'Lamplit Art Society is a non-profit collective sparking connection and creativity through shared lamp art. More at '),
                TextSpan(
                  text: 'lamplit.ca',
                  style: TextStyle(
                    color: BrandColors.auroraBlue,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                TextSpan(text: '.'),
              ],
            ),
          ),
        ),
        const SizedBox(height: 24),
        // Two-column layout: live nearby on the left, persisted-but-not-
        // currently-broadcasting on the right. Both columns share the
        // outer ListView's scroll, so an unbalanced count (lots of seen,
        // few nearby) lands the page scrollable rather than clipping.
        Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Expanded(
              child: _NearbyColumn(
                nearby: nearby,
                connected: connected,
                currentLampId: widget.lampId,
                inventoryById: inventoryById,
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _SeenColumn(seen: seenOnly, now: now),
            ),
          ],
        ),
      ],
    );
  }
}

class _NearbyColumn extends StatelessWidget {
  const _NearbyColumn({
    required this.nearby,
    required this.connected,
    required this.currentLampId,
    required this.inventoryById,
  });

  final List<NearbyLamp> nearby;
  final bool connected;
  final String currentLampId;
  final Map<String, InventoryLamp> inventoryById;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'Nearby lamps',
          style: TextStyle(
            color: BrandColors.headerYellow,
            fontSize: 13,
            fontWeight: FontWeight.w700,
            letterSpacing: 0.8,
          ),
        ),
        if (nearby.isEmpty)
          const Padding(
            padding: EdgeInsets.symmetric(vertical: 12),
            child: Text(
              'No lamps heard yet.',
              style: TextStyle(color: BrandColors.fogGrey, fontSize: 12),
            ),
          )
        else
          for (final l in nearby)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 6),
              child: Row(
                children: [
                  StatusDot(
                    kind: statusFor(
                      lampId: l.id,
                      nearby: nearby,
                      connected: connected && l.id == currentLampId,
                    ),
                    size: 10,
                  ),
                  const SizedBox(width: 8),
                  Builder(builder: (_) {
                    final colors = resolveLampColors(
                      inv: inventoryById[l.id],
                      near: l,
                    );
                    return LampIcon(
                      shade: colors.shade,
                      base: colors.base,
                      size: 22,
                    );
                  }),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      l.name.isEmpty ? '(unnamed)' : l.name,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        color: BrandColors.lampWhite,
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                ],
              ),
            ),
      ],
    );
  }
}

class _SeenColumn extends StatelessWidget {
  const _SeenColumn({required this.seen, required this.now});

  final List<SeenLamp> seen;
  final DateTime now;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'Seen before',
          style: TextStyle(
            color: BrandColors.headerYellow,
            fontSize: 13,
            fontWeight: FontWeight.w700,
            letterSpacing: 0.8,
          ),
        ),
        if (seen.isEmpty)
          const Padding(
            padding: EdgeInsets.symmetric(vertical: 12),
            child: Text(
              'None yet.',
              style: TextStyle(color: BrandColors.fogGrey, fontSize: 12),
            ),
          )
        else
          for (final l in seen)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 6),
              child: Row(
                children: [
                  // Wrap the persisted RGB ints back into Color for
                  // LampIcon. We don't have an InventoryLamp / NearbyLamp
                  // for these rows, so resolveLampColors isn't the fit.
                  LampIcon(
                    shade: Color(0xFF000000 | l.shadeRgb),
                    base: Color(0xFF000000 | l.baseRgb),
                    size: 22,
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          l.name.isEmpty ? '(unnamed)' : l.name,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: const TextStyle(
                            color: BrandColors.lampWhite,
                            fontSize: 13,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                        Text(
                          formatLastSeen(l.lastSeenEpochMs, now),
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: const TextStyle(
                            color: BrandColors.slateGrey,
                            fontSize: 11,
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
      ],
    );
  }
}

/// Lamplit brand mark — the SVG glyph from `assets/lamplit-logo.svg` plus
/// the "Lamplit Art Society" sub-wordmark. The tap target for the 5-tap
/// advanced-settings unlock wraps this whole column.
class _LamplitWordmark extends StatelessWidget {
  const _LamplitWordmark();

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        SvgPicture.asset(
          'assets/lamplit-logo.svg',
          height: 140,
          colorFilter: const ColorFilter.mode(
              BrandColors.lampWhite, BlendMode.srcIn),
          semanticsLabel: 'Lamplit logo',
        ),
        const SizedBox(height: 14),
        const Text(
          'Lamplit Art Society',
          style: TextStyle(
            color: BrandColors.headerYellow,
            fontSize: 12,
            fontWeight: FontWeight.w600,
            letterSpacing: 4,
          ),
        ),
      ],
    );
  }
}
