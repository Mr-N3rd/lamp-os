import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/friendly_error.dart';
import '../../../core/widgets/settings_row.dart';
import '../../control/application/control_notifier.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../application/wisp_notifier.dart';
import '../domain/wisp_status.dart';

/// Wisp tab — picks which Aurora zone the wisp follows for this fleet.
///
/// The wisp is a separate ESP32-C6 node that subscribes to Aurora and
/// repaints the mesh with whatever palette the picked zone emits. From
/// the lamp app's perspective it's an opaque peer; we talk to it through
/// the lamp's BLE control service which proxies wispOps onto the mesh
/// and caches the wisp's status broadcasts back at us.
///
/// Layout (top-down):
///   1. Header — wisp MAC + last-seen freshness
///   2. Connection chips — WiFi / Aurora
///   3. Current-zone callout + zoneSource provenance
///   4. Observed-zones picker (chip list, tap to setZone)
///   5. Clear-selection button (visible only when an app/nvs pin exists)
///   6. Palette prefix indicator
///   7. WiFi config placeholder ("coming soon")
class WispPane extends ConsumerWidget {
  const WispPane({super.key, required this.lampId});

  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final controlAsync = ref.watch(controlNotifierProvider(lampId));
    return controlAsync.when(
      loading: () => ConnectingView(deviceId: lampId),
      error: (e, _) => FriendlyError.page(
        title: "Couldn't reach your lamp.",
        subtitle:
            "They may have wandered out of range. Bring your phone closer "
            'and try again.',
        rawError: e,
      ),
      data: (state) {
        if (!state.connected) {
          return ConnectingView(deviceId: lampId);
        }
        return _WispBody(lampId: lampId);
      },
    );
  }
}

class _WispBody extends ConsumerStatefulWidget {
  const _WispBody({required this.lampId});
  final String lampId;

  @override
  ConsumerState<_WispBody> createState() => _WispBodyState();
}

class _WispBodyState extends ConsumerState<_WispBody> {
  /// Phone-local epoch ms at the most recent wispStatus notify. Used to
  /// derive a "Xs ago" indicator without trusting the wisp's own
  /// `lastSeenMs` (which is wisp millis, resets on wisp reboot, and is
  /// useless for the human-time "is the wisp still alive?" question).
  ///
  /// Seeded eagerly on first non-empty status; refreshed by the
  /// `ref.listen` below on every subsequent state change.
  int? _lastNotifyEpochMs;

  /// 1Hz heartbeat that rebuilds the "Xs ago" label so it counts up
  /// even while no new notifies are arriving. Cancelled on dispose.
  Timer? _staleTickTimer;

  @override
  void initState() {
    super.initState();
    _staleTickTimer = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _staleTickTimer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Stamp the local clock whenever a new status lands so the "Xs ago"
    // computation has a fresh anchor. Tracking it here (rather than in
    // the notifier) keeps the phone-local time concern out of the
    // domain layer — the notifier holds only what the wisp reported.
    ref.listen<AsyncValue<WispStatus>>(
      wispNotifierProvider(widget.lampId),
      (_, next) {
        if (next is AsyncData<WispStatus> && next.value.present) {
          _lastNotifyEpochMs = DateTime.now().millisecondsSinceEpoch;
        }
      },
    );

    final async = ref.watch(wispNotifierProvider(widget.lampId));
    return async.when(
      loading: () => const Center(
        child: CircularProgressIndicator(color: BrandColors.fogGrey),
      ),
      error: (e, _) => FriendlyError.page(
        title: "Couldn't read wisp status.",
        subtitle:
            "The lamp returned a wisp status read error. Try switching "
            'tabs and back.',
        rawError: e,
      ),
      data: (status) => _buildBody(context, status),
    );
  }

  Widget _buildBody(BuildContext context, WispStatus status) {
    final notifier = ref.read(wispNotifierProvider(widget.lampId).notifier);
    final stale = _staleSeconds(status);

    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 16, 16, 32),
      children: [
        _WispHeader(status: status, staleSeconds: stale),
        const SizedBox(height: 16),
        _ConnectionChips(status: status),
        const SizedBox(height: 24),
        _CurrentZone(status: status),
        const SizedBox(height: 16),
        _ObservedZonesPicker(
          status: status,
          onPickZone: (z) => _runWispOp(() => notifier.setZone(z)),
        ),
        if (_canClearSelection(status)) ...[
          const SizedBox(height: 16),
          Align(
            alignment: Alignment.centerLeft,
            child: TextButton.icon(
              onPressed: () => _runWispOp(notifier.clearZone),
              icon: const Icon(Icons.close, size: 16),
              label: const Text('Clear selection'),
              style: TextButton.styleFrom(
                foregroundColor: BrandColors.fogGrey,
              ),
            ),
          ),
        ],
        if (status.paletteIdPrefix.isNotEmpty) ...[
          const SizedBox(height: 16),
          _PalettePrefixChip(prefix: status.paletteIdPrefix),
        ],
        const SizedBox(height: 24),
        const SettingsGroupHeading('Wisp setup'),
        const SettingsRow(
          icon: Icons.wifi,
          title: 'WiFi config',
          subtitle: 'Coming soon',
          // onTap omitted → no chevron, row reads as disabled.
        ),
      ],
    );
  }

  /// Seconds since the last notify in phone-local time, or null when no
  /// notify has been recorded yet. Used by the header to badge stale
  /// status; threshold-based "is it stale?" lives at the caller.
  int? _staleSeconds(WispStatus status) {
    if (!status.present) return null;
    if (_lastNotifyEpochMs == null) return null;
    final delta = DateTime.now().millisecondsSinceEpoch - _lastNotifyEpochMs!;
    return delta ~/ 1000;
  }

  /// Only show the "Clear selection" button when the operator actually
  /// has a pin to clear — clearing a `firstSeen` or `none` source is a
  /// no-op on the wisp side and would just confuse the UI.
  bool _canClearSelection(WispStatus s) =>
      s.zoneSource == 'appOp' || s.zoneSource == 'nvs';

  /// Runs a wispOp (setZone/clearZone) and surfaces failures as a
  /// SnackBar. Without this the notifier's optimistic update would
  /// stick around forever on a failed write — no status notify is
  /// coming back to reconcile it.
  Future<void> _runWispOp(Future<void> Function() op) async {
    try {
      await op();
    } catch (_) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text("Couldn't reach the wisp — try again."),
        ),
      );
    }
  }
}

class _WispHeader extends StatelessWidget {
  const _WispHeader({required this.status, required this.staleSeconds});
  final WispStatus status;
  final int? staleSeconds;

  static const _staleThresholdSec = 60;

  @override
  Widget build(BuildContext context) {
    if (!status.present) {
      return const Padding(
        padding: EdgeInsets.symmetric(vertical: 16),
        child: Text(
          'No wisp detected.',
          style: TextStyle(color: BrandColors.fogGrey, fontSize: 14),
        ),
      );
    }
    final mac = status.wispMac ?? '';
    final stale =
        staleSeconds != null && staleSeconds! > _staleThresholdSec;
    final freshnessLabel = staleSeconds == null
        ? 'Just connected'
        : 'Last seen ${staleSeconds}s ago';
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            const Icon(Icons.bubble_chart,
                color: BrandColors.auroraBlue, size: 22),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                mac,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(
                  color: BrandColors.lampWhite,
                  fontSize: 16,
                  fontWeight: FontWeight.w600,
                  letterSpacing: 0.5,
                ),
              ),
            ),
          ],
        ),
        const SizedBox(height: 4),
        Text(
          freshnessLabel,
          style: TextStyle(
            color: stale ? BrandColors.error : BrandColors.fogGrey,
            fontSize: 12,
            fontStyle: stale ? FontStyle.italic : FontStyle.normal,
          ),
        ),
      ],
    );
  }
}

class _ConnectionChips extends StatelessWidget {
  const _ConnectionChips({required this.status});
  final WispStatus status;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        _StatusChip(
          icon: Icons.wifi,
          label: status.wifiConnected
              ? 'WiFi: Connected'
              : 'WiFi: Disconnected',
          connected: status.wifiConnected,
        ),
        const SizedBox(width: 8),
        _StatusChip(
          icon: Icons.auto_awesome,
          label: status.auroraConnected
              ? 'Aurora: Connected'
              : 'Aurora: Disconnected',
          connected: status.auroraConnected,
        ),
      ],
    );
  }
}

class _StatusChip extends StatelessWidget {
  const _StatusChip({
    required this.icon,
    required this.label,
    required this.connected,
  });
  final IconData icon;
  final String label;
  final bool connected;

  @override
  Widget build(BuildContext context) {
    final fg = connected ? BrandColors.lumenGreen : BrandColors.error;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: fg.withValues(alpha: 0.16),
        borderRadius: BorderRadius.circular(999),
        border: Border.all(color: fg.withValues(alpha: 0.6)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 14, color: fg),
          const SizedBox(width: 6),
          Text(
            label,
            style: TextStyle(
              color: fg,
              fontSize: 12,
              fontWeight: FontWeight.w600,
            ),
          ),
        ],
      ),
    );
  }
}

class _CurrentZone extends StatelessWidget {
  const _CurrentZone({required this.status});
  final WispStatus status;

  @override
  Widget build(BuildContext context) {
    final String headline;
    final String? subhead;
    if (status.currentZone == null) {
      headline = 'No zone selected';
      subhead = status.zoneSource == 'none'
          ? 'Tap a zone below to assign one.'
          : null;
    } else {
      headline = 'Zone ${status.currentZone}';
      subhead = _zoneSourceLabel(status.zoneSource);
    }
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'CURRENT ZONE',
          style: TextStyle(
            color: BrandColors.headerYellow,
            fontSize: 11,
            fontWeight: FontWeight.w700,
            letterSpacing: 1.2,
          ),
        ),
        const SizedBox(height: 8),
        Text(
          headline,
          style: const TextStyle(
            color: BrandColors.lampWhite,
            fontSize: 24,
            fontWeight: FontWeight.w700,
          ),
        ),
        if (subhead != null) ...[
          const SizedBox(height: 4),
          Text(
            subhead,
            style: const TextStyle(
              color: BrandColors.fogGrey,
              fontSize: 12,
              fontStyle: FontStyle.italic,
            ),
          ),
        ],
      ],
    );
  }

  /// Maps the wire-format `zoneSource` enum to the parenthetical
  /// "where did this come from?" sub-label on the current-zone card.
  static String _zoneSourceLabel(String source) {
    switch (source) {
      case 'appOp':
        return 'Set in app';
      case 'nvs':
        return 'Persisted on the wisp';
      case 'firstSeen':
        return 'First zone heard on the mesh';
      case 'none':
      default:
        return '';
    }
  }
}

class _ObservedZonesPicker extends StatelessWidget {
  const _ObservedZonesPicker({
    required this.status,
    required this.onPickZone,
  });
  final WispStatus status;
  final ValueChanged<int> onPickZone;

  @override
  Widget build(BuildContext context) {
    final zones = [...status.observedZones]..sort();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'OBSERVED ZONES',
          style: TextStyle(
            color: BrandColors.headerYellow,
            fontSize: 11,
            fontWeight: FontWeight.w700,
            letterSpacing: 1.2,
          ),
        ),
        const SizedBox(height: 8),
        if (zones.isEmpty)
          const Text(
            "No zones heard yet. Once Aurora starts publishing zone "
            "palettes on the mesh, they'll appear here.",
            style: TextStyle(color: BrandColors.fogGrey, fontSize: 12),
          )
        else
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              for (final z in zones)
                _ZoneChip(
                  zoneId: z,
                  selected: z == status.currentZone,
                  onTap: () => onPickZone(z),
                ),
            ],
          ),
      ],
    );
  }
}

class _ZoneChip extends StatelessWidget {
  const _ZoneChip({
    required this.zoneId,
    required this.selected,
    required this.onTap,
  });
  final int zoneId;
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
    return Material(
      color: Colors.transparent,
      child: InkWell(
        borderRadius: BorderRadius.circular(999),
        onTap: onTap,
        child: Container(
          padding:
              const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
          decoration: BoxDecoration(
            color: fill,
            borderRadius: BorderRadius.circular(999),
            border: Border.all(
              color: border,
              width: selected ? 2 : 1,
            ),
          ),
          child: Text(
            'Zone $zoneId',
            style: TextStyle(
              color: fg,
              fontSize: 14,
              fontWeight: selected ? FontWeight.w700 : FontWeight.w500,
            ),
          ),
        ),
      ),
    );
  }
}

class _PalettePrefixChip extends StatelessWidget {
  const _PalettePrefixChip({required this.prefix});
  final String prefix;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: BrandColors.auroraBlue.withValues(alpha: 0.16),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(
          color: BrandColors.auroraBlue.withValues(alpha: 0.4),
        ),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Icon(Icons.palette_outlined,
              size: 14, color: BrandColors.auroraBlue),
          const SizedBox(width: 6),
          Text(
            'Palette $prefix',
            style: const TextStyle(
              color: BrandColors.auroraBlue,
              fontSize: 12,
              fontFamily: 'monospace',
              fontWeight: FontWeight.w600,
            ),
          ),
        ],
      ),
    );
  }
}
