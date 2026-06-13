import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/lamp_icon.dart';
import '../../control/application/control_notifier.dart';
import '../../nearby/application/nearby_lamps_notifier.dart';
import '../application/dispositions_notifier.dart';
import '../domain/social_mode.dart';

/// Social tab: lamp personality + per-peer disposition list.
///
/// Top: a row of chunky personality pills (Introvert / Ambivert /
/// Extrovert), same visual style as the Shade/Base/Both picker in the
/// add-expression flow. Bound to ControlNotifier.setLampSocialMode —
/// change rides through the next Save Changes. No timings exposed,
/// whimsical-by-design.
///
/// Below: every lamp currently nearby (live BLE). Disposition is only
/// meaningful when the peer is actually here — historical "seen" lamps
/// are not shown here. Each row carries a 5-position slider with a
/// single active-position label on the right (salty → wary →
/// neutral → fond → smitten) wired to the dispositions provider.
/// Writes are debounced 500ms after the last slider movement and
/// flushed on tab leave so a drag-then-navigate doesn't drop the edit.
class SocialScreen extends ConsumerWidget {
  const SocialScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // .select() so this screen only rebuilds when (mode, selfName, hasState)
    // changes — NOT on every slider tick or color drag that goes through
    // controlNotifierProvider (audit perf-H1).
    final ctl = ref.watch(controlNotifierProvider(lampId).select((a) {
      final lamp = a.value?.lamp;
      return (
        hasState: lamp != null,
        mode: lamp?.socialMode ?? SocialMode.ambivert,
        selfName: lamp?.name ?? '',
      );
    }));
    if (!ctl.hasState) {
      return const Center(
        child: CircularProgressIndicator(color: BrandColors.fogGrey),
      );
    }
    final notifier = ref.read(controlNotifierProvider(lampId).notifier);
    final mode = ctl.mode;
    final selfName = ctl.selfName;

    final nearby = ref.watch(nearbyLampsNotifierProvider);

    // Nearby-only — skip unnamed (disposition is keyed by name, and
    // Config::setDisposition rejects empty-string keys firmware-side).
    // Skip self via BOTH id-match AND name-match — the BLE scan id format
    // (raw MAC on Android, UUID on iOS) doesn't always equal the inventory
    // deviceId we're connected through. Name-match is the reliable fallback.
    // Trade-off: if a second lamp happens to share this lamp's name, it'll
    // also be filtered out; the alternative (jacko sees itself) is worse.
    // NOTE: disposition lookup/write uses the lamp's user-set name. A
    // renamed peer's disposition orphans (slice-1 acknowledged
    // limitation; see social-tab spec).
    final rows = <_SocialLampRow>[
      for (final l in nearby)
        if (l.name.isNotEmpty && l.id != lampId && l.name != selfName)
          _SocialLampRow(
            id: l.id,
            name: l.name,
            baseRgb: l.baseRgb,
            shadeRgb: l.shadeRgb,
          ),
    ];

    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 16, 16, 32),
      children: [
        const Text(
          'Personality',
          style: TextStyle(
              color: BrandColors.lampWhite, fontSize: 14),
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            _PersonalityButton(
              label: 'Introvert',
              icon: Icons.bedtime_outlined,
              selected: mode == SocialMode.introvert,
              onTap: () => notifier.setLampSocialMode(SocialMode.introvert),
            ),
            const SizedBox(width: 8),
            _PersonalityButton(
              label: 'Ambivert',
              icon: Icons.balance,
              selected: mode == SocialMode.ambivert,
              onTap: () => notifier.setLampSocialMode(SocialMode.ambivert),
            ),
            const SizedBox(width: 8),
            _PersonalityButton(
              label: 'Extrovert',
              icon: Icons.celebration_outlined,
              selected: mode == SocialMode.extrovert,
              onTap: () => notifier.setLampSocialMode(SocialMode.extrovert),
            ),
          ],
        ),
        const SizedBox(height: 16),
        const Padding(
          padding: EdgeInsets.symmetric(horizontal: 4),
          child: Text(
            'Lamps notice the company they keep. How this one greets, '
            'glows, and settles shifts a little with the lamps it meets — '
            'and with how it feels about each of them.',
            style: TextStyle(
              color: BrandColors.fogGrey,
              fontSize: 12,
              fontStyle: FontStyle.italic,
              height: 1.4,
            ),
          ),
        ),
        const SizedBox(height: 16),
        const Text(
          'Nearby lamps',
          style: TextStyle(color: BrandColors.lampWhite, fontSize: 14),
        ),
        const SizedBox(height: 4),
        if (rows.isEmpty)
          const Padding(
            padding: EdgeInsets.symmetric(vertical: 24),
            child: Center(
              child: Text(
                'No lamps nearby right now. Once others wander by, '
                "they'll show up here.",
                textAlign: TextAlign.center,
                style: TextStyle(
                    color: BrandColors.fogGrey, fontSize: 12),
              ),
            ),
          )
        else
          for (final row in rows)
            _LampDispositionRow(lampId: lampId, row: row),
      ],
    );
  }
}

class _SocialLampRow {
  const _SocialLampRow({
    required this.id,
    required this.name,
    required this.baseRgb,
    required this.shadeRgb,
  });
  final String id;
  final String name;
  final int baseRgb;
  final int shadeRgb;
}

class _LampDispositionRow extends ConsumerWidget {
  const _LampDispositionRow({required this.lampId, required this.row});
  final String lampId;
  final _SocialLampRow row;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final dispNotifier = ref.read(dispositionsProvider(lampId).notifier);
    // Watch so the row rebuilds + the active label updates as the slider
    // moves. Notifier holds an in-memory copy, so this is cheap.
    ref.watch(dispositionsProvider(lampId));
    final disposition = dispNotifier.get(row.name);

    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              LampIcon(
                shade: Color(0xFF000000 | row.shadeRgb),
                base: Color(0xFF000000 | row.baseRgb),
                size: 22,
              ),
              const SizedBox(width: 10),
              Expanded(
                child: Text(
                  row.name,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ),
            ],
          ),
          Row(
            children: [
              Expanded(
                child: Slider(
                  value: disposition.toDouble(),
                  min: 1,
                  max: 5,
                  divisions: 4,
                  onChanged: (v) =>
                      dispNotifier.set(row.name, v.round()),
                ),
              ),
              const SizedBox(width: 8),
              // Single active-position label — deliberately breaks the
              // usual "endpoint legend" pattern. Whimsical-by-design:
              // the word changes as you drag and reveals the continuum
              // through interaction rather than upfront labelling.
              SizedBox(
                width: 64,
                child: Text(
                  _dispositionLabel(disposition),
                  textAlign: TextAlign.right,
                  style: const TextStyle(
                    color: BrandColors.fogGrey,
                    fontSize: 12,
                    fontStyle: FontStyle.italic,
                  ),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

/// Big chunky pill — same visual style as `_TargetButton` in
/// `add_expression_picker_screen.dart` (shade/base/both). Renders the
/// active mode with a glowPink fill so it reads at a glance.
class _PersonalityButton extends StatelessWidget {
  const _PersonalityButton({
    required this.label,
    required this.icon,
    required this.selected,
    required this.onTap,
  });

  final String label;
  final IconData icon;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final fill = selected ? BrandColors.glowPink : Colors.transparent;
    final border = selected
        ? BrandColors.glowPink
        : BrandColors.slateGrey.withValues(alpha: 0.5);
    final fg =
        selected ? BrandColors.midnightBlack : BrandColors.lampWhite;
    return Expanded(
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: onTap,
          borderRadius: BorderRadius.circular(12),
          child: Container(
            height: 72,
            decoration: BoxDecoration(
              color: fill,
              border: Border.all(
                color: border,
                width: selected ? 2 : 1,
              ),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Icon(icon, color: fg, size: 24),
                const SizedBox(height: 4),
                Text(
                  label,
                  style: TextStyle(
                    color: fg,
                    fontSize: 14,
                    fontWeight:
                        selected ? FontWeight.w700 : FontWeight.w500,
                    letterSpacing: 0.5,
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// Cute/playful escalation describing how the lamp feels about the named
/// peer — from "salty" (1) to "smitten" (5). Used as the single active-
/// position label on the disposition slider. Values outside 1..5 fall
/// back to neutral.
String _dispositionLabel(int value) {
  switch (value) {
    case 1:
      return 'salty';
    case 2:
      return 'wary';
    case 4:
      return 'fond';
    case 5:
      return 'smitten';
    case 3:
    default:
      return 'neutral';
  }
}
