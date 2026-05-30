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
import '../application/lamp_status.dart';
import 'expressions_screen.dart';
import 'info_screen.dart';
import 'setup_screen.dart';

/// Diagonal aurora-blue → glow-pink gradient used on the active tab
/// indicator and the AppBar Save action. Ported from the prior Vue app's
/// `TopNavigation.vue` and `Lamp.vue` chrome.
const _brandGradient = LinearGradient(
  begin: Alignment.topLeft,
  end: Alignment.bottomRight,
  colors: [BrandColors.auroraBlue, BrandColors.glowPink],
);

enum LampTab { control, expressions, setup, info }

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

    final body = switch (_tab) {
      LampTab.control => ControlScreen(lampId: widget.lampId),
      LampTab.expressions => ExpressionsScreen(lampId: widget.lampId),
      LampTab.setup => SetupScreen(lampId: widget.lampId),
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
    final status = statusFor(
      lampId: widget.lampId,
      nearby: nearby,
      connected: connected,
    );

    return Scaffold(
      appBar: AppBar(
        title: LampChip(
          name: name,
          status: status,
          onTap: () => showLampPickerSheet(
            context,
            currentLampId: widget.lampId,
          ),
        ),
        actions: [
          // Save is visible on Control and Setup. Setup edits (name, home
          // WiFi, MQTT, advanced) ride the same isDirty + settings-blob
          // save flow. Expressions has its own per-entry persistence via
          // CHAR_EXPRESSION_OP, and Info is read-only — neither needs a
          // global Save.
          if (_tab == LampTab.control || _tab == LampTab.setup)
            _SaveAction(lampId: widget.lampId),
        ],
      ),
      body: body,
      bottomNavigationBar: NavigationBarTheme(
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
          selectedIndex: _tab.index,
          onDestinationSelected: (i) =>
              setState(() => _tab = LampTab.values[i]),
          destinations: [
            _gradientDestination(Icons.tune, 'Control', _tab == LampTab.control),
            _gradientDestination(
                Icons.auto_awesome, 'Expressions', _tab == LampTab.expressions),
            _gradientDestination(
                Icons.settings, 'Setup', _tab == LampTab.setup),
            _gradientDestination(
                Icons.info_outline, 'Info', _tab == LampTab.info),
          ],
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
    final connected = state?.connected ?? false;
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
        child: Stack(
          clipBehavior: Clip.none,
          children: [
            InkWell(
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
            Positioned(
              top: -2,
              right: -2,
              child: Container(
                key: const ValueKey('save-dirty-dot'),
                width: 8,
                height: 8,
                decoration: BoxDecoration(
                  color: BrandColors.error,
                  shape: BoxShape.circle,
                  border:
                      Border.all(color: BrandColors.lampWhite, width: 1.2),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
