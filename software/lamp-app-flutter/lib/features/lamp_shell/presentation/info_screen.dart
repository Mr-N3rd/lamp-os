import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/utils/tap_counter.dart';
import '../../../core/widgets/info_panel.dart';
import '../../../core/widgets/lamp_icon.dart';
import '../../../core/widgets/status_dot.dart';
import '../../control/application/control_notifier.dart';
import '../../nearby/application/nearby_lamps_notifier.dart';
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
        final notifier =
            ref.read(controlNotifierProvider(widget.lampId).notifier);
        notifier.setLampAdvancedEnabled(true);
        if (context.mounted) {
          GoRouter.maybeOf(context)
              ?.push(AppRoutes.advancedLeds(widget.lampId));
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
    final connected = ref.watch(controlNotifierProvider(widget.lampId)
        .select((async) => async.value?.connected ?? false));
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
        const Row(
          children: [
            Text(
              'Nearby lamps',
              style: TextStyle(
                color: BrandColors.headerYellow,
                fontSize: 13,
                fontWeight: FontWeight.w700,
                letterSpacing: 0.8,
              ),
            ),
            Spacer(),
          ],
        ),
        if (nearby.isEmpty)
          const Padding(
            padding: EdgeInsets.symmetric(vertical: 12),
            child: Text(
              'No other lamps heard yet. Lamps within Bluetooth range '
              'will appear here.',
              style: TextStyle(color: BrandColors.fogGrey, fontSize: 12),
            ),
          )
        else
          ...nearby.map((l) {
            final status = statusFor(
              lampId: l.id,
              nearby: nearby,
              connected: connected && l.id == widget.lampId,
            );
            return Padding(
              padding: const EdgeInsets.symmetric(vertical: 6),
              child: Row(
                children: [
                  StatusDot(kind: status, size: 12),
                  const SizedBox(width: 10),
                  LampIcon(
                    shade: Color(0xFF000000 | l.shadeRgb),
                    base: Color(0xFF000000 | l.baseRgb),
                    size: 26,
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: Text(
                      l.name.isEmpty ? '(unnamed)' : l.name,
                      style: const TextStyle(
                        color: BrandColors.lampWhite,
                        fontSize: 14,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                ],
              ),
            );
          }),
      ],
    );
  }
}

/// Stylised text logo. Tap target for the secret 5-tap unlock. We don't
/// ship a vector asset yet — the wordmark is good enough until we drop
/// `lamplit-logo.svg` into `assets/`.
class _LamplitWordmark extends StatelessWidget {
  const _LamplitWordmark();

  @override
  Widget build(BuildContext context) {
    return const Column(
      children: [
        Text(
          'lamplit',
          style: TextStyle(
            color: BrandColors.lampWhite,
            fontSize: 38,
            fontWeight: FontWeight.w900,
            letterSpacing: 1.2,
          ),
        ),
        SizedBox(height: 2),
        Text(
          'ART SOCIETY',
          style: TextStyle(
            color: BrandColors.headerYellow,
            fontSize: 11,
            fontWeight: FontWeight.w600,
            letterSpacing: 6,
          ),
        ),
      ],
    );
  }
}
