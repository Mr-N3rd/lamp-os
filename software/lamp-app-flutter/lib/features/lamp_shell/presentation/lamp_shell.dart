import 'package:flutter/material.dart';

import '../../../features/control/presentation/control_screen.dart';
import 'expressions_placeholder.dart';
import 'setup_placeholder.dart';

enum LampTab { control, expressions, setup }

class LampShell extends StatefulWidget {
  const LampShell({
    super.key,
    required this.lampId,
    this.initialTab = LampTab.control,
  });

  final String lampId;
  final LampTab initialTab;

  @override
  State<LampShell> createState() => _LampShellState();
}

class _LampShellState extends State<LampShell> {
  late LampTab _tab = widget.initialTab;

  @override
  Widget build(BuildContext context) {
    final body = switch (_tab) {
      LampTab.control => ControlScreen(lampId: widget.lampId),
      LampTab.expressions => ExpressionsPlaceholder(lampId: widget.lampId),
      LampTab.setup => SetupPlaceholder(lampId: widget.lampId),
    };

    return Scaffold(
      appBar: AppBar(title: Text(widget.lampId)),
      body: body,
      bottomNavigationBar: NavigationBar(
        selectedIndex: _tab.index,
        onDestinationSelected: (i) =>
            setState(() => _tab = LampTab.values[i]),
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.tune),
            label: 'Control',
          ),
          NavigationDestination(
            icon: Icon(Icons.auto_awesome),
            label: 'Expressions',
          ),
          NavigationDestination(
            icon: Icon(Icons.settings),
            label: 'Setup',
          ),
        ],
      ),
    );
  }
}
