import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/widgets/lamp_chip.dart';
import '../../../features/control/application/control_notifier.dart';
import '../../../features/control/presentation/control_screen.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../../inventory/presentation/widgets/lamp_picker_sheet.dart';
import '../../nearby/application/nearby_lamps_notifier.dart';
import '../application/lamp_status.dart';
import 'expressions_screen.dart';
import 'setup_screen.dart';

enum LampTab { control, expressions, setup }

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
    };

    final inventory = ref.watch(inventoryNotifierProvider).value;
    final name = inventory
            ?.firstWhereOrNull((l) => l.id == widget.lampId)
            ?.name ??
        widget.lampId;

    final nearby = ref.watch(nearbyLampsNotifierProvider);
    final connected = ref
            .watch(controlNotifierProvider(widget.lampId))
            .value
            ?.connected ??
        false;
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
          // Save is visible on both Control and Setup — Setup edits (name,
          // home WiFi, MQTT, advanced) ride the same isDirty + settings-blob
          // save flow. Expressions has its own per-entry persistence via
          // CHAR_EXPRESSION_OP so no global Save is needed there.
          if (_tab != LampTab.expressions) _SaveAction(lampId: widget.lampId),
        ],
      ),
      body: body,
      bottomNavigationBar: NavigationBar(
        selectedIndex: _tab.index,
        onDestinationSelected: (i) =>
            setState(() => _tab = LampTab.values[i]),
        destinations: const [
          NavigationDestination(icon: Icon(Icons.tune), label: 'Control'),
          NavigationDestination(
              icon: Icon(Icons.auto_awesome), label: 'Expressions'),
          NavigationDestination(icon: Icon(Icons.settings), label: 'Setup'),
        ],
      ),
    );
  }
}

class _SaveAction extends ConsumerWidget {
  const _SaveAction({required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // Watching the AsyncValue so any state change rebuilds the icon; the
    // isDirty getter is read off the notifier, not from the value itself.
    final async = ref.watch(controlNotifierProvider(lampId));
    final notifier = ref.read(controlNotifierProvider(lampId).notifier);
    final state = async.value;
    final connected = state?.connected ?? false;
    final canSave = state != null && connected && notifier.isDirty;
    final String tooltip;
    if (!connected) {
      tooltip = 'Reconnecting…';
    } else if (!notifier.isDirty) {
      tooltip = 'No changes to save';
    } else {
      tooltip = 'Save';
    }
    return IconButton(
      icon: const Icon(Icons.save_outlined),
      tooltip: tooltip,
      onPressed: canSave ? notifier.save : null,
    );
  }
}
