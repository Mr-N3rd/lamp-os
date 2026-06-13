import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/app_snackbar.dart';
import '../../../core/widgets/friendly_error.dart';
import '../../../core/widgets/password_prompt_dialog.dart';
import '../../../core/widgets/settings_row.dart';
import '../../control/application/control_notifier.dart';
import '../../control/domain/lamp_color.dart';
import '../../control/presentation/widgets/color_picker_sheet.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../../lamp_shell/presentation/widgets/wifi_network_picker.dart';
import '../application/wisp_notifier.dart';
import '../domain/wisp_source_mode.dart';
import '../domain/wisp_status.dart';
import '../domain/zone_source.dart';
import 'palette_gradient_bar.dart';

/// Wisp tab — controls how the wisp drives the lamp grid's paint.
///
/// The wisp is a separate ESP32-C6 node that can either follow an Aurora
/// zone (subscription palette) or repaint the mesh from an operator-
/// defined palette. From the lamp app's perspective it's an opaque peer;
/// we talk to it through the lamp's BLE control service which proxies
/// wispOps onto the mesh and caches the wisp's status broadcasts back
/// at us.
///
/// Layout (top-down):
///   0. Palette gradient bar — full-width, no padding, mirrors the wisp's
///      30-pixel NeoPixel ring. Off/Aurora-without-palette fall back to
///      warm-white; Manual previews the editor's current draft live.
///   1. Header — wisp MAC + last-seen freshness
///   2. Connection chips — WiFi / Aurora
///   3. Source pill picker — Off / Manual / Aurora (Aurora disabled
///      until at least one zone has been observed)
///   4. Manual palette editor (visible when source == manual) — up to
///      10 colors, drag-to-reorder, swipe-to-delete, tap-to-edit,
///      explicit Save button gates broadcast
///   5. Current-zone callout + zoneSource provenance (Aurora mode)
///   6. Observed-zones picker (chip list, tap to setZone — Aurora mode)
///   7. Clear-selection button (visible only when an app/nvs pin exists)
///   8. Palette prefix indicator (wisp's last published palette)
///   9. WiFi config — tappable row that opens the lamp's scanned-network
///      picker (shared with Home Mode) → password prompt → setWifi op
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
    ref.listen<AsyncValue<WispStatus>>(wispNotifierProvider(widget.lampId), (
      _,
      next,
    ) {
      if (next is AsyncData<WispStatus> && next.value.present) {
        _lastNotifyEpochMs = DateTime.now().millisecondsSinceEpoch;
      }
    });

    final async = ref.watch(wispNotifierProvider(widget.lampId));
    return async.when(
      loading: () => const Center(
        child: CircularProgressIndicator(color: BrandColors.fogGrey),
      ),
      // A read error here almost always means "this lamp doesn't have
      // the wisp characteristic" — pre-FriendlyError, this dead-ended
      // a user on a non-wisp lamp who switched to the Wisp tab. Now
      // we render the same empty-state body as for `status.present ==
      // false`, which surfaces the "No wisp detected" guidance and
      // keeps the tab usable. Audit ux-H4. True catastrophic errors
      // (BLE disconnect during read) get the same UI, which is fine —
      // the user's next action is reconnect, and the rest of the app
      // handles that out-of-band.
      error: (_, _) => _buildBody(context, WispStatus.empty),
      data: (status) => _buildBody(context, status),
    );
  }

  Widget _buildBody(BuildContext context, WispStatus status) {
    final notifier = ref.read(wispNotifierProvider(widget.lampId).notifier);
    final stale = _staleSeconds(status);
    final source = status.source;
    final auroraEnabled = status.auroraDetected;

    // First time we render in Manual mode in this session, seed the
    // draft from whatever the notifier currently has as `saved` so the
    // editor doesn't open empty if the user already saved earlier in
    // the same session. Idempotent: re-seeding with the same list is
    // a no-op for the dirty check.
    if (source == WispSourceMode.manual &&
        notifier.draftManualPalette.isEmpty &&
        notifier.savedManualPalette.isNotEmpty) {
      // Schedule on the next frame so we don't mutate notifier state
      // mid-build (Riverpod would assert).
      WidgetsBinding.instance.addPostFrameCallback((_) {
        notifier.resetManualPaletteDraft();
      });
    }

    // Gradient bar lives outside the ListView's padded interior so it
    // can stretch edge-to-edge. The ListView keeps its 16px gutter for
    // the rest of the content; the bar sits flush above the header.
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        PaletteGradientBar(
          sourceMode: source,
          manualPalette: notifier.draftManualPalette,
        ),
        Expanded(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(16, 16, 16, 32),
            children: [
              _WispHeader(status: status, staleSeconds: stale),
              const SizedBox(height: 16),
              _ConnectionChips(status: status),
              const SizedBox(height: 16),
              _SourcePicker(
                current: source,
                auroraEnabled: auroraEnabled,
                onSelect: (m) => _runWispOp(() => notifier.setSource(m)),
              ),
              if (source == WispSourceMode.manual) ...[
                const SizedBox(height: 20),
                _ManualPaletteEditor(lampId: widget.lampId),
              ],
              if (source == WispSourceMode.aurora) ...[
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
              ],
              if (status.paletteIdPrefix.isNotEmpty &&
                  source != WispSourceMode.off) ...[
                const SizedBox(height: 16),
                _PalettePrefixChip(prefix: status.paletteIdPrefix),
              ],
              const SizedBox(height: 24),
              const SettingsGroupHeading('Wisp setup'),
              _WifiConfigRow(lampId: widget.lampId, status: status),
            ],
          ),
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
      s.zoneSource == ZoneSource.appOp || s.zoneSource == ZoneSource.nvs;

  /// Runs a wispOp (setZone/clearZone) and surfaces failures as a
  /// SnackBar. Without this the notifier's optimistic update would
  /// stick around forever on a failed write — no status notify is
  /// coming back to reconcile it.
  Future<void> _runWispOp(Future<void> Function() op) async {
    try {
      await op();
    } catch (_) {
      if (!mounted) return;
      AppSnackbar.error(context, "Couldn't reach the wisp — try again.");
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
    final stale = staleSeconds != null && staleSeconds! > _staleThresholdSec;
    final freshnessLabel = staleSeconds == null
        ? 'Just connected'
        : 'Last seen ${staleSeconds}s ago';
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            const Icon(
              Icons.bubble_chart,
              color: BrandColors.auroraBlue,
              size: 22,
            ),
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
      subhead = status.zoneSource == ZoneSource.none
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

  /// Maps the `zoneSource` enum to the parenthetical "where did this
  /// come from?" sub-label on the current-zone card.
  static String _zoneSourceLabel(ZoneSource source) {
    switch (source) {
      case ZoneSource.appOp:
        return 'Set in app';
      case ZoneSource.nvs:
        return 'Persisted on the wisp';
      case ZoneSource.firstSeen:
        return 'First zone heard on the mesh';
      case ZoneSource.none:
      case ZoneSource.unknown:
        return '';
    }
  }
}

class _ObservedZonesPicker extends StatelessWidget {
  const _ObservedZonesPicker({required this.status, required this.onPickZone});
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
    final fg = selected ? BrandColors.midnightBlack : BrandColors.lampWhite;
    return Material(
      color: Colors.transparent,
      child: InkWell(
        borderRadius: BorderRadius.circular(999),
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
          decoration: BoxDecoration(
            color: fill,
            borderRadius: BorderRadius.circular(999),
            border: Border.all(color: border, width: selected ? 2 : 1),
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
          const Icon(
            Icons.palette_outlined,
            size: 14,
            color: BrandColors.auroraBlue,
          ),
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

// ── Source picker ─────────────────────────────────────────────────────
// Chunky pill picker matching the Social tab's personality picker
// (see features/social/presentation/social_screen.dart's
// _PersonalityButton). Aurora is disabled until the wisp has actually
// observed a zone — otherwise the operator could flip to Aurora and
// stare at an unexplained-blank screen for ~minutes while the wisp
// discovers an Aurora server.
class _SourcePicker extends StatelessWidget {
  const _SourcePicker({
    required this.current,
    required this.auroraEnabled,
    required this.onSelect,
  });

  final WispSourceMode current;
  final bool auroraEnabled;
  final ValueChanged<WispSourceMode> onSelect;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'Source',
          style: TextStyle(color: BrandColors.lampWhite, fontSize: 14),
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            _SourcePill(
              label: 'Off',
              icon: Icons.power_settings_new,
              selected: current == WispSourceMode.off,
              enabled: true,
              onTap: () => onSelect(WispSourceMode.off),
            ),
            const SizedBox(width: 8),
            _SourcePill(
              label: 'Manual',
              icon: Icons.palette_outlined,
              selected: current == WispSourceMode.manual,
              enabled: true,
              onTap: () => onSelect(WispSourceMode.manual),
            ),
            const SizedBox(width: 8),
            _SourcePill(
              label: 'Aurora',
              icon: Icons.auto_awesome,
              selected: current == WispSourceMode.aurora,
              enabled: auroraEnabled,
              onTap: () => onSelect(WispSourceMode.aurora),
            ),
          ],
        ),
        if (!auroraEnabled) ...[
          const SizedBox(height: 6),
          const Padding(
            padding: EdgeInsets.symmetric(horizontal: 2),
            child: Text(
              "No Aurora zone heard yet. Once a zone shows up on the "
              "mesh, you'll be able to follow it.",
              style: TextStyle(
                color: BrandColors.fogGrey,
                fontSize: 11,
                fontStyle: FontStyle.italic,
              ),
            ),
          ),
        ],
      ],
    );
  }
}

class _SourcePill extends StatelessWidget {
  const _SourcePill({
    required this.label,
    required this.icon,
    required this.selected,
    required this.enabled,
    required this.onTap,
  });

  final String label;
  final IconData icon;
  final bool selected;
  final bool enabled;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    // Visual contract mirrors the Social tab's _PersonalityButton:
    // selected → solid glowPink fill on midnight text; idle → slateGrey
    // outline on lampWhite. Disabled drops opacity instead of changing
    // hue so the row reads as "not for you right now" rather than
    // "broken".
    final Color fill = enabled && selected
        ? BrandColors.glowPink
        : Colors.transparent;
    final Color border = enabled && selected
        ? BrandColors.glowPink
        : BrandColors.slateGrey.withValues(alpha: enabled ? 0.5 : 0.25);
    final Color fg = enabled && selected
        ? BrandColors.midnightBlack
        : BrandColors.lampWhite.withValues(alpha: enabled ? 1.0 : 0.4);

    return Expanded(
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          onTap: enabled ? onTap : null,
          borderRadius: BorderRadius.circular(12),
          child: Container(
            height: 72,
            decoration: BoxDecoration(
              color: fill,
              border: Border.all(
                color: border,
                width: selected && enabled ? 2 : 1,
              ),
              borderRadius: BorderRadius.circular(12),
            ),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Icon(icon, color: fg, size: 22),
                const SizedBox(height: 4),
                Text(
                  label,
                  style: TextStyle(
                    color: fg,
                    fontSize: 13,
                    fontWeight: selected && enabled
                        ? FontWeight.w700
                        : FontWeight.w500,
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

// ── Manual palette editor ─────────────────────────────────────────────
// Horizontal swatch row + add/edit/reorder/delete + explicit save.
//
// Visual rhyme with the Aurora `_PalettePrefixChip` above: same auroraBlue
// border treatment around the gradient strip, same monospace label
// underneath. The strip itself is just N equal-width swatches laid out
// side-by-side — closer to a "color stops" preview than a true gradient,
// but it matches how the wisp will actually paint the lamps (it samples
// the palette as discrete colors per peer, not blended).
class _ManualPaletteEditor extends ConsumerStatefulWidget {
  const _ManualPaletteEditor({required this.lampId});
  final String lampId;

  @override
  ConsumerState<_ManualPaletteEditor> createState() =>
      _ManualPaletteEditorState();
}

class _ManualPaletteEditorState extends ConsumerState<_ManualPaletteEditor> {
  bool _saving = false;

  @override
  Widget build(BuildContext context) {
    final notifier = ref.read(wispNotifierProvider(widget.lampId).notifier);
    // Watch so the editor rebuilds when the notifier emits draft / save
    // state changes via _bumpState. We don't care about the WispStatus
    // payload here — the source-picker switch is enough to get us into
    // this widget at all.
    ref.watch(wispNotifierProvider(widget.lampId));

    final draft = notifier.draftManualPalette;
    final dirty = notifier.manualPaletteDirty;
    final atCap = draft.length >= 10;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text(
          'MANUAL PALETTE',
          style: TextStyle(
            color: BrandColors.headerYellow,
            fontSize: 11,
            fontWeight: FontWeight.w700,
            letterSpacing: 1.2,
          ),
        ),
        const SizedBox(height: 8),
        _PaletteStrip(
          colors: draft,
          onTapSwatch: (i) => _editAt(i),
          onRemove: (i) => notifier.removeManualPaletteColor(i),
          onReorder: notifier.reorderManualPaletteColor,
        ),
        const SizedBox(height: 12),
        Row(
          children: [
            OutlinedButton.icon(
              onPressed: atCap ? null : _addNew,
              icon: const Icon(Icons.add, size: 16),
              label: Text(atCap ? 'Max 10' : 'Add color'),
              style: OutlinedButton.styleFrom(
                foregroundColor: atCap
                    ? BrandColors.fogGrey
                    : BrandColors.lampWhite,
                side: BorderSide(
                  color: BrandColors.slateGrey.withValues(
                    alpha: atCap ? 0.25 : 0.5,
                  ),
                ),
              ),
            ),
            const Spacer(),
            FilledButton.icon(
              onPressed: (dirty && !_saving) ? _save : null,
              icon: _saving
                  ? const SizedBox(
                      width: 14,
                      height: 14,
                      child: CircularProgressIndicator(
                        strokeWidth: 2,
                        color: BrandColors.midnightBlack,
                      ),
                    )
                  : const Icon(Icons.check, size: 16),
              label: const Text('Save'),
              style: FilledButton.styleFrom(
                backgroundColor: BrandColors.glowPink,
                foregroundColor: BrandColors.midnightBlack,
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        const Padding(
          padding: EdgeInsets.symmetric(horizontal: 2),
          child: Text(
            'Edits are local until you tap Save. Swipe a swatch to '
            'remove, long-press to drag.',
            style: TextStyle(
              color: BrandColors.fogGrey,
              fontSize: 11,
              fontStyle: FontStyle.italic,
            ),
          ),
        ),
      ],
    );
  }

  Future<void> _addNew() async {
    final notifier = ref.read(wispNotifierProvider(widget.lampId).notifier);
    // Default new swatch — pure white (RGB=255). Hue-saturated would feel
    // editorial; white reads as "blank slate, pick me".
    const initial = LampColor(r: 255, g: 255, b: 255, w: 0);
    final picked = await showColorPickerSheet(
      context,
      initial: initial,
      title: 'Add palette color',
      bpp: 3, // RGB only — wisp manual palette has no W channel.
    );
    if (picked == null) return;
    notifier.appendManualPaletteColor(picked);
  }

  Future<void> _editAt(int index) async {
    final notifier = ref.read(wispNotifierProvider(widget.lampId).notifier);
    final draft = notifier.draftManualPalette;
    if (index < 0 || index >= draft.length) return;
    final picked = await showColorPickerSheet(
      context,
      initial: draft[index],
      title: 'Edit palette color',
      bpp: 3,
    );
    if (picked == null) return;
    notifier.updateManualPaletteColor(index, picked);
  }

  Future<void> _save() async {
    setState(() => _saving = true);
    final notifier = ref.read(wispNotifierProvider(widget.lampId).notifier);
    try {
      await notifier.setManualPalette();
      if (!mounted) return;
      AppSnackbar.info(context, 'Palette saved.');
    } catch (_) {
      if (!mounted) return;
      AppSnackbar.error(
        context, "Couldn't reach the wisp — try Save again.",
      );
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }
}

class _PaletteStrip extends StatelessWidget {
  const _PaletteStrip({
    required this.colors,
    required this.onTapSwatch,
    required this.onRemove,
    required this.onReorder,
  });

  final List<LampColor> colors;
  final ValueChanged<int> onTapSwatch;
  final ValueChanged<int> onRemove;
  final void Function(int oldIndex, int newIndex) onReorder;

  @override
  Widget build(BuildContext context) {
    if (colors.isEmpty) {
      return Container(
        height: 56,
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(8),
          border: Border.all(
            color: BrandColors.slateGrey.withValues(alpha: 0.35),
            style: BorderStyle.solid,
          ),
        ),
        alignment: Alignment.center,
        child: const Text(
          'No colors yet — tap Add to start your palette.',
          style: TextStyle(color: BrandColors.fogGrey, fontSize: 12),
        ),
      );
    }

    // ReorderableListView gives us drag-handles + long-press reorder.
    // Horizontal scroll direction keeps the gradient-like row layout.
    return Container(
      height: 56,
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(8),
        border: Border.all(
          color: BrandColors.auroraBlue.withValues(alpha: 0.4),
        ),
      ),
      clipBehavior: Clip.antiAlias,
      child: ReorderableListView.builder(
        scrollDirection: Axis.horizontal,
        buildDefaultDragHandles: false,
        itemCount: colors.length,
        // Strip the lift-shadow tint that ReorderableListView paints on
        // top of the picked-up item — the swatch is opaque and the
        // shadow looks like a bug otherwise.
        proxyDecorator: (child, _, _) =>
            Material(color: Colors.transparent, elevation: 4, child: child),
        onReorder: onReorder,
        itemBuilder: (context, i) {
          final c = colors[i];
          return _PaletteSwatch(
            key: ValueKey('manual-swatch-$i-${c.r}-${c.g}-${c.b}'),
            color: c,
            onTap: () => onTapSwatch(i),
            onDelete: () => onRemove(i),
            index: i,
          );
        },
      ),
    );
  }
}

// ── WiFi config row ───────────────────────────────────────────────────
// Tappable settings row that opens the lamp's network-picker sheet
// (shared with Home Mode — the lamp owns the scan radio, the wisp does
// not). Picking a network opens a password prompt; on confirm the
// credentials ship through the existing `setWifi` wispOp.
//
// No optimistic UI: a wrong password or out-of-range AP would leave a
// permanent "Connected" badge if we flipped state ourselves. The row
// subtitle echoes `WispStatus.wifiConnected` so the operator sees the
// authoritative state without scrolling back up to the chip.
class _WifiConfigRow extends ConsumerStatefulWidget {
  const _WifiConfigRow({required this.lampId, required this.status});
  final String lampId;
  final WispStatus status;

  @override
  ConsumerState<_WifiConfigRow> createState() => _WifiConfigRowState();
}

class _WifiConfigRowState extends ConsumerState<_WifiConfigRow> {
  bool _busy = false;

  @override
  Widget build(BuildContext context) {
    final subtitle = _busy
        ? 'Sending credentials to wisp…'
        : (widget.status.wifiConnected
            ? 'Connected — tap to change network'
            : 'Not connected — tap to configure');
    return SettingsRow(
      key: const Key('wifi-config-row'),
      icon: Icons.wifi,
      title: 'WiFi',
      subtitle: subtitle,
      onTap: _busy ? null : _openPicker,
    );
  }

  Future<void> _openPicker() async {
    final picked = await showWifiPickerSheet(
      context,
      lampId: widget.lampId,
    );
    if (picked == null) return;
    if (!mounted) return;
    // Prompt for the password. Open networks could theoretically skip
    // this, but in practice we still want a confirm step before shipping
    // the creds — and the wisp's wifi op takes a `pw` field regardless.
    final pw = await showPasswordPromptDialog(
      context,
      title: 'Password for ${picked.ssid}',
      subtitle: picked.encrypted
          ? 'Enter the WiFi password to share with the wisp.'
          : 'This network appears to be open. Leave blank or enter '
              'a password if required.',
      confirmLabel: 'Save',
    );
    if (pw == null) return;
    if (!mounted) return;

    setState(() => _busy = true);
    final notifier = ref.read(wispNotifierProvider(widget.lampId).notifier);
    try {
      await notifier.setWifi(picked.ssid, pw);
      if (!mounted) return;
      AppSnackbar.info(
        context, 'Wi-Fi creds sent to wisp (${picked.ssid}).',
      );
    } catch (_) {
      if (!mounted) return;
      AppSnackbar.error(
        context, "Couldn't reach the wisp — try again.",
      );
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }
}

class _PaletteSwatch extends StatelessWidget {
  const _PaletteSwatch({
    super.key,
    required this.color,
    required this.onTap,
    required this.onDelete,
    required this.index,
  });

  final LampColor color;
  final VoidCallback onTap;
  final VoidCallback onDelete;
  final int index;

  @override
  Widget build(BuildContext context) {
    return ReorderableDelayedDragStartListener(
      index: index,
      child: Dismissible(
        key: ValueKey('dismiss-$index-${color.toHex()}'),
        direction: DismissDirection.vertical,
        onDismissed: (_) => onDelete(),
        background: Container(
          color: BrandColors.error.withValues(alpha: 0.3),
          alignment: Alignment.center,
          child: const Icon(Icons.delete_outline, color: Colors.white),
        ),
        child: InkWell(
          onTap: onTap,
          child: Container(width: 48, color: color.toSwatch()),
        ),
      ),
    );
  }
}
