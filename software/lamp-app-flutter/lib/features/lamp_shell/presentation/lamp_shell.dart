import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/lamp_chip.dart';
import '../../../features/control/application/control_notifier.dart';
import '../../../features/control/presentation/control_screen.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../../inventory/presentation/widgets/lamp_picker_sheet.dart';
import '../../nearby/application/nearby_lamps_notifier.dart';
import '../../nearby/application/scan_grace_provider.dart';
import '../application/lamp_status.dart';
import '../../social/presentation/social_screen.dart';
import '../../wisp/application/wisp_notifier.dart';
import 'expressions_screen.dart';
import 'info_screen.dart';

/// Diagonal aurora-blue → glow-pink gradient used on the active tab
/// indicator and the AppBar Save action. Ported from the prior Vue app's
/// `TopNavigation.vue` and `Lamp.vue` chrome.
const _brandGradient = LinearGradient(
  begin: Alignment.topLeft,
  end: Alignment.bottomRight,
  colors: [BrandColors.auroraBlue, BrandColors.glowPink],
);

/// Bottom-nav tabs for the lamp shell. Wisp used to be a bottom-nav
/// destination but moved out: when only one wisp is painting a given
/// lamp (enforced by the wisp-side multi-wisp coordination), the floating
/// orb indicator is already onscreen advertising it. Tapping the orbs
/// five times unlocks the dedicated wisp config route (`/lamp/:id/wisp`).
/// Same gesture pattern as the Lamplit-wordmark advanced-unlock.
enum LampTab { control, expressions, social, info }

class LampShell extends ConsumerStatefulWidget {
  const LampShell({
    super.key,
    required this.lampId,
    this.initialTab = LampTab.control,
  });

  final String lampId;
  final LampTab initialTab;

  @override
  ConsumerState<LampShell> createState() => _LampShellState();
}

class _LampShellState extends ConsumerState<LampShell> {
  late LampTab _tab = widget.initialTab;

  @override
  Widget build(BuildContext context) {
    // Keep the control connection alive across tab switches. Without this
    // watch, switching to Expressions or Setup unmounts ControlScreen, drops
    // the only listener on controlNotifierProvider, and the provider
    // auto-disposes (incl. ble.disconnect). LampShell unmounting (back to
    // inventory, swap to another lamp) still cleans up because this watch
    // is released with the shell.
    ref.watch(controlNotifierProvider(widget.lampId));

    // Same lifecycle treatment for the wisp notifier even though the
    // Wisp tab is gone from the bottom nav: the WispIndicator on the
    // Setup tab still consumes it, and the dedicated wisp config route
    // pushed from the 5-tap-orbs gesture should reuse the same
    // notifier instance (manual-palette draft + source mode). Without
    // this lamp-shell-level watch, the indicator would dispose the
    // notifier the moment the user navigated away from the Setup tab.
    ref.watch(wispNotifierProvider(widget.lampId));

    final body = switch (_tab) {
      LampTab.control => ControlScreen(lampId: widget.lampId),
      LampTab.expressions => ExpressionsScreen(lampId: widget.lampId),
      LampTab.social => SocialScreen(lampId: widget.lampId),
      LampTab.info => InfoScreen(lampId: widget.lampId),
    };

    final inventory = ref.watch(inventoryNotifierProvider).value;
    final name = inventory
            ?.firstWhereOrNull((l) => l.id == widget.lampId)
            ?.name ??
        widget.lampId;

    final nearby = ref.watch(nearbyLampsNotifierProvider);
    // `select` so the shell only rebuilds when the connection state
    // actually flips — not on every shade/base color tick during a drag.
    final connected = ref.watch(controlNotifierProvider(widget.lampId)
        .select((async) => async.value?.connected ?? false));
    final inScanGrace = ref.watch(scanGraceActiveProvider);
    final status = statusFor(
      lampId: widget.lampId,
      nearby: nearby,
      connected: connected,
      inScanGrace: inScanGrace,
    );

    return Scaffold(
      appBar: AppBar(
        // The LampChip in `title` already opens the switcher modal on tap
        // — that's the on-screen nav. Skip GoRouter's auto-injected back
        // arrow (now present because LampShell is pushed on top of My
        // Lamps) so the AppBar doesn't carry two redundant nav controls.
        // Android system back-gesture still pops to My Lamps.
        automaticallyImplyLeading: false,
        title: LampChip(
          name: name,
          status: status,
          onTap: () => showLampPickerSheet(
            context,
            currentLampId: widget.lampId,
          ),
        ),
        actions: [
          // Save pill — visible on tabs that ride the isDirty +
          // settings-blob save flow. Info is read-only (branding +
          // firmware + version) so the pill is hidden there. The Wisp
          // bottom-nav tab is gone entirely; the dedicated wisp config
          // screen (pushed via the 5-tap-orbs gesture) is its own
          // route with its own AppBar.
          if (_tab != LampTab.info) _SaveAction(lampId: widget.lampId),
        ],
      ),
      body: body,
      // Tab nav is gated on the BLE connection. When the link is
      // down (post-disconnect, mid-reconnect), the per-tab views would
      // either render stale data or hang on a write the lamp can't
      // hear — both confusing. Greying + ignoring the buttons makes
      // the reconnect-in-flight state visible without taking the user
      // off the page they were on. ConnectionBanner (at the top of the
      // tab body) carries the attempt counter; this is the
      // complementary affordance on the bottom nav.
      bottomNavigationBar: IgnorePointer(
        ignoring: !connected,
        child: Opacity(
          opacity: connected ? 1.0 : 0.4,
          child: NavigationBarTheme(
        data: NavigationBarThemeData(
          // Vue active state: `linear-gradient(135deg, auroraBlue, glowPink)`
          // with a soft shadow. Material 3's NavigationBar only lets us set
          // a flat indicator color, so we render the gradient via a custom
          // indicator BoxDecoration.
          indicatorColor: Colors.transparent,
          indicatorShape: const StadiumBorder(),
          labelTextStyle: WidgetStateProperty.resolveWith((states) {
            if (states.contains(WidgetState.selected)) {
              return const TextStyle(
                color: BrandColors.lampWhite,
                fontWeight: FontWeight.w600,
                fontSize: 12,
              );
            }
            return const TextStyle(
              color: BrandColors.slateGrey,
              fontSize: 12,
            );
          }),
          iconTheme: WidgetStateProperty.resolveWith((states) {
            return IconThemeData(
              color: states.contains(WidgetState.selected)
                  ? BrandColors.lampWhite
                  : BrandColors.slateGrey,
            );
          }),
        ),
        child: NavigationBar(
          // Bottom nav was carrying 6 destinations; "Setup" and "Info"
          // are now reached via the AppBar Configuration gear (Setup =
          // lamp-config hub; Info = About section at its bottom). That
          // leaves four primary modes — the first tab is relabelled
          // "Setup" since it's where most lamp tuning happens.
          selectedIndex: _tab.index,
          onDestinationSelected: (i) =>
              setState(() => _tab = LampTab.values[i]),
          destinations: [
            _gradientDestination(Icons.tune, 'Setup', _tab == LampTab.control),
            _gradientDestination(
                Icons.auto_awesome, 'Expressions', _tab == LampTab.expressions),
            _gradientDestination(Icons.handshake_outlined, 'Social',
                _tab == LampTab.social),
            _gradientDestination(
                Icons.info_outline, 'Info', _tab == LampTab.info),
          ],
        ),
      ),
        ),
      ),
    );
  }
}

NavigationDestination _gradientDestination(
    IconData icon, String label, bool selected) {
  final iconWidget = Icon(icon, size: 22);
  return NavigationDestination(
    icon: selected
        ? Container(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
            decoration: BoxDecoration(
              gradient: _brandGradient,
              borderRadius: BorderRadius.circular(999),
              boxShadow: [
                BoxShadow(
                  color: BrandColors.auroraBlue.withValues(alpha: 0.3),
                  blurRadius: 8,
                  offset: const Offset(0, 2),
                ),
              ],
            ),
            child: iconWidget,
          )
        : iconWidget,
    label: label,
  );
}

class _SaveAction extends ConsumerWidget {
  const _SaveAction({required this.lampId});
  final String lampId;

  // Visual dimensions are tuned to match LampChip on the left of the
  // AppBar (padding h=12 v=6, text size 12 w600, icon ~14) so the two
  // pills read as a consistent pair.
  static Widget _outlined({
    required String label,
    required IconData icon,
    required VoidCallback? onPressed,
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: OutlinedButton.icon(
        style: OutlinedButton.styleFrom(
          foregroundColor: BrandColors.fogGrey,
          side: BorderSide(
              color: BrandColors.lampWhite.withValues(alpha: 0.18)),
          padding:
              const EdgeInsets.symmetric(horizontal: 12, vertical: 0),
          minimumSize: const Size(0, 32),
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          textStyle: const TextStyle(
            fontSize: 12,
            fontWeight: FontWeight.w600,
          ),
        ),
        onPressed: onPressed,
        icon: Icon(icon, size: 14),
        label: Text(label),
      ),
    );
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // Watching the AsyncValue so any state change rebuilds the widget; the
    // isDirty getter is read off the notifier, not from the value itself.
    final async = ref.watch(controlNotifierProvider(lampId));
    final notifier = ref.read(controlNotifierProvider(lampId).notifier);
    final state = async.value;
    // AsyncLoading (state == null) means build() is still running — we've
    // never been connected to this lamp in this session. "Connecting…"
    // matches that mental model; "Reconnecting…" is reserved for the
    // post-disconnect recovery path so the two states stay legible.
    if (state == null) {
      return Tooltip(
        message: 'Connecting to this lamp…',
        child: _outlined(
            label: 'Connecting…',
            icon: Icons.bluetooth_searching,
            onPressed: null),
      );
    }
    final connected = state.connected;
    if (!connected) {
      return Tooltip(
        message: 'Reconnecting to this lamp…',
        child: _outlined(
            label: 'Reconnecting…',
            icon: Icons.cloud_off,
            onPressed: null),
      );
    }
    if (!notifier.isDirty) {
      return Tooltip(
        message: 'All changes saved',
        child: _outlined(
            label: 'Saved', icon: Icons.check, onPressed: null),
      );
    }
    return Tooltip(
      message: 'You have unsaved changes — tap to save',
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
        child: InkWell(
          borderRadius: BorderRadius.circular(999),
          onTap: notifier.save,
          child: Container(
            padding:
                const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
            decoration: BoxDecoration(
              gradient: _brandGradient,
              borderRadius: BorderRadius.circular(999),
              boxShadow: [
                BoxShadow(
                  color: BrandColors.glowPink.withValues(alpha: 0.45),
                  blurRadius: 10,
                  spreadRadius: 1,
                  offset: const Offset(0, 1),
                ),
              ],
            ),
            child: const Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(Icons.save, size: 14, color: BrandColors.lampWhite),
                SizedBox(width: 8),
                Text('Save changes',
                    style: TextStyle(
                      color: BrandColors.lampWhite,
                      fontWeight: FontWeight.w600,
                      fontSize: 12,
                    )),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
