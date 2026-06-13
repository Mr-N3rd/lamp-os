import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_svg/flutter_svg.dart';
import 'package:go_router/go_router.dart';
import 'package:package_info_plus/package_info_plus.dart';

import '../../../core/app_channel.dart';
import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/utils/tap_counter.dart';
import '../../../core/widgets/info_panel.dart';
import '../../control/application/advanced_session.dart';
import '../../control/application/control_notifier.dart';

/// Info tab — Lamplit branding only. Nearby + Seen lamp lists moved to
/// the new Social tab (see `features/social/presentation/social_screen.dart`).
/// The Lamplit wordmark here is the tap-5-times target that unlocks the
/// session-only advanced flag.
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
          // home-mode rows alongside the now-unlocked Advanced LED row.
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
    // Firmware identity rides the cached CHAR_LAMP_SECTION read. While the
    // notifier is still loading (BLE connect + per-section reads in flight)
    // or has errored out, fall back to a "..." placeholder so the screen
    // never flashes a misleading "Firmware 0.0.0 ()". Legacy firmware
    // that pre-dates the fwVersion/fwChannel fields lands in the same
    // placeholder branch via the null check below.
    final async = ref.watch(controlNotifierProvider(widget.lampId));
    final fwLine = async.maybeWhen(
      data: (state) {
        final v = state.lamp.fwVersion;
        final ch = state.lamp.fwChannel;
        if (v == null || ch == null) return 'Firmware ...';
        return 'Firmware ${formatFirmwareSemver(v)} ($ch)';
      },
      orElse: () => 'Firmware ...',
    );

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
        // Version footer — firmware first (read from CHAR_LAMP_SECTION),
        // then this app's version (PackageInfo + hardcoded kAppChannel).
        Center(
          child: Text(
            fwLine,
            style: const TextStyle(
              color: BrandColors.fogGrey,
              fontSize: 12,
            ),
          ),
        ),
        const SizedBox(height: 4),
        Center(
          child: FutureBuilder<PackageInfo>(
            future: PackageInfo.fromPlatform(),
            builder: (context, snap) {
              final info = snap.data;
              final v = info != null
                  ? '${info.version}+${info.buildNumber} ($kAppChannel)'
                  : '...';
              return Text(
                'App $v',
                style: const TextStyle(
                  color: BrandColors.fogGrey,
                  fontSize: 12,
                ),
              );
            },
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
