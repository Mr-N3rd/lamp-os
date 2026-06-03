import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/lamp_icon.dart';
import '../../control/application/control_notifier.dart';
import '../../inventory/domain/last_seen.dart';
import '../../nearby/application/nearby_lamps_notifier.dart';
import '../../nearby/application/seen_lamps_notifier.dart';
import '../application/dispositions_notifier.dart';
import '../domain/social_mode.dart';

/// Social tab: lamp personality + the per-peer disposition list.
///
/// Top row: SegmentedButton with Introvert / Ambivert / Extrovert. Bound
/// to ControlNotifier.setLampSocialMode — change rides through the next
/// Save Changes. No timings shown to the user (whimsical-by-design,
/// per spec).
///
/// Below: every lamp this lamp has heard, dedup'd by id, sorted by most
/// recently seen. Each row carries a 5-position disposition slider
/// (salty .. neutral .. smitten) wired to the dispositions provider —
/// reads CHAR_SOCIAL_DISPOSITIONS on tab open, writes back debounced
/// 500ms after the last slider movement.
class SocialScreen extends ConsumerWidget {
  const SocialScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final async = ref.watch(controlNotifierProvider(lampId));
    final state = async.value;
    if (state == null) {
      return const Center(
        child: CircularProgressIndicator(color: BrandColors.fogGrey),
      );
    }
    final notifier = ref.read(controlNotifierProvider(lampId).notifier);
    final mode = state.lamp.socialMode;

    final nearby = ref.watch(nearbyLampsNotifierProvider);
    final seen = ref.watch(seenLampsNotifierProvider);
    final now = DateTime.now();

    // Merge nearby + seen, prefer nearby (live) for overlapping ids,
    // sort by most-recently-seen first.
    final byId = <String, _SocialLampRow>{};
    for (final l in nearby) {
      byId[l.id] = _SocialLampRow(
        id: l.id,
        name: l.name,
        baseRgb: l.baseRgb,
        shadeRgb: l.shadeRgb,
        lastSeenEpochMs: l.lastSeenEpochMs,
        live: true,
      );
    }
    for (final l in seen) {
      if (byId.containsKey(l.id)) continue;
      byId[l.id] = _SocialLampRow(
        id: l.id,
        name: l.name,
        baseRgb: l.baseRgb,
        shadeRgb: l.shadeRgb,
        lastSeenEpochMs: l.lastSeenEpochMs,
        live: false,
      );
    }
    final rows = byId.values.toList()
      ..sort((a, b) => b.lastSeenEpochMs.compareTo(a.lastSeenEpochMs));

    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 16, 16, 32),
      children: [
        const Text(
          'Personality',
          style: TextStyle(
              color: BrandColors.lampWhite, fontSize: 14),
        ),
        const SizedBox(height: 8),
        Center(
          child: SegmentedButton<SocialMode>(
            showSelectedIcon: false,
            segments: const [
              ButtonSegment(
                value: SocialMode.introvert,
                label: Text('Introvert'),
              ),
              ButtonSegment(
                value: SocialMode.ambivert,
                label: Text('Ambivert'),
              ),
              ButtonSegment(
                value: SocialMode.extrovert,
                label: Text('Extrovert'),
              ),
            ],
            selected: {mode},
            onSelectionChanged: (s) =>
                notifier.setLampSocialMode(s.first),
          ),
        ),
        const SizedBox(height: 24),
        const Text(
          'Lamps you’ve met',
          style: TextStyle(color: BrandColors.lampWhite, fontSize: 14),
        ),
        const SizedBox(height: 4),
        if (rows.isEmpty)
          const Padding(
            padding: EdgeInsets.symmetric(vertical: 24),
            child: Center(
              child: Text(
                'No lamps yet. Once others wander by, they’ll show up here.',
                textAlign: TextAlign.center,
                style: TextStyle(
                    color: BrandColors.fogGrey, fontSize: 12),
              ),
            ),
          )
        else
          for (final row in rows)
            _LampDispositionRow(
              lampId: lampId,
              row: row,
              now: now,
            ),
      ],
    );
  }
}

/// Unified row data — coalesced from NearbyLamp + SeenLamp so the row
/// widget doesn't have to know which source it came from. `live` flips
/// the rendering of the last-seen text ("now" vs "5m ago") but isn't
/// load-bearing beyond that.
class _SocialLampRow {
  const _SocialLampRow({
    required this.id,
    required this.name,
    required this.baseRgb,
    required this.shadeRgb,
    required this.lastSeenEpochMs,
    required this.live,
  });
  final String id;
  final String name;
  final int baseRgb;
  final int shadeRgb;
  final int lastSeenEpochMs;
  final bool live;
}

class _LampDispositionRow extends ConsumerWidget {
  const _LampDispositionRow({
    required this.lampId,
    required this.row,
    required this.now,
  });
  final String lampId;
  final _SocialLampRow row;
  final DateTime now;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final dispNotifier = ref.read(dispositionsProvider(lampId).notifier);
    // Watch so the row rebuilds when the local map updates (after a
    // slider change). The notifier holds an in-memory copy so this is
    // a cheap rebuild.
    ref.watch(dispositionsProvider(lampId));
    final disposition = dispNotifier.get(row.name);
    final displayName = row.name.isEmpty ? '(unnamed)' : row.name;
    final lastSeenText = row.live
        ? 'now'
        : formatLastSeen(row.lastSeenEpochMs, now);

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
                  displayName,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ),
              Text(
                lastSeenText,
                style: const TextStyle(
                  color: BrandColors.slateGrey,
                  fontSize: 11,
                ),
              ),
            ],
          ),
          Row(
            children: [
              const Text('salty',
                  style: TextStyle(
                      color: BrandColors.fogGrey, fontSize: 11)),
              Expanded(
                child: Slider(
                  value: disposition.toDouble(),
                  min: 1,
                  max: 5,
                  divisions: 4,
                  onChanged: row.name.isEmpty
                      ? null
                      : (v) => dispNotifier.set(row.name, v.round()),
                ),
              ),
              const Text('smitten',
                  style: TextStyle(
                      color: BrandColors.fogGrey, fontSize: 11)),
            ],
          ),
        ],
      ),
    );
  }
}
