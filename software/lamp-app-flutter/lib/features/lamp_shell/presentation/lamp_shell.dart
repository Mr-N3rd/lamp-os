import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../features/control/application/control_notifier.dart';
import '../../../features/control/presentation/control_screen.dart';
import '../../inventory/application/inventory_notifier.dart';
import 'expressions_placeholder.dart';
import 'setup_placeholder.dart';

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
      LampTab.expressions => ExpressionsPlaceholder(lampId: widget.lampId),
      LampTab.setup => SetupPlaceholder(lampId: widget.lampId),
    };

    final inventory = ref.watch(inventoryNotifierProvider).value;
    final name = inventory
            ?.firstWhereOrNull((l) => l.id == widget.lampId)
            ?.name ??
        widget.lampId;

    return Scaffold(
      appBar: AppBar(
        title: Text(name),
        actions: [
          if (_tab == LampTab.control) _SaveAction(lampId: widget.lampId),
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
