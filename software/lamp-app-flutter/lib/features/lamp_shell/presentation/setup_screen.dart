import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/settings_row.dart';
import '../../control/application/control_notifier.dart';
import '../../control/application/control_state.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../application/wifi_notifier.dart';

/// Setup tab — mobile-style row list. Tapping a row drills into a
/// sub-pane (HomeWifi, HomeMode, AdvancedLeds, Knockout). The Home Wi-Fi
/// row has a tap-toggle that flips the SSID between "" and a placeholder
/// without leaving the screen; tapping the chevron-y area drills in to
/// pick the network / enter the password.
class SetupScreen extends ConsumerWidget {
  const SetupScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final async = ref.watch(controlNotifierProvider(lampId));
    return async.when(
      loading: () => ConnectingView(deviceId: lampId),
      error: (e, _) => Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Text(
            'Could not reach this lamp: $e',
            style: const TextStyle(color: BrandColors.fogGrey),
            textAlign: TextAlign.center,
          ),
        ),
      ),
      data: (state) => _SetupBody(lampId: lampId, state: state),
    );
  }
}

class _SetupBody extends ConsumerWidget {
  const _SetupBody({required this.lampId, required this.state});
  final String lampId;
  final ControlState state;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final n = ref.read(controlNotifierProvider(lampId).notifier);
    final homeOn = state.home.ssid.isNotEmpty;
    return ListView(
      padding: const EdgeInsets.only(top: 8, bottom: 32),
      children: [
        const SettingsGroupHeading('Lamp'),
        SettingsRow(
          icon: Icons.label_outline,
          title: 'Name',
          subtitle: state.lamp.name.isEmpty ? '(unnamed)' : state.lamp.name,
          onTap: () => _showRenameDialog(context, state.lamp.name, n.setLampName),
        ),
        const SettingsGroupHeading('Connectivity'),
        SettingsRow(
          icon: Icons.home_outlined,
          title: 'Home Mode',
          subtitle: homeOn
              ? '${state.home.ssid} · ${state.home.brightness}%'
              : 'Off',
          trailing: Switch(
            value: homeOn,
            // Toggling off forgets the saved Wi-Fi (firmware drops creds).
            // Toggling on drills into the Home Mode pane where the user
            // picks a network from the live scan.
            onChanged: (v) {
              if (!v) {
                ref.read(wifiNotifierProvider(lampId).notifier).forget();
                n.setHomeSsid('');
                n.setHomePassword('');
              } else {
                context.push(AppRoutes.homeMode(lampId));
              }
            },
          ),
          onTap: () => context.push(AppRoutes.homeMode(lampId)),
        ),
        const SettingsGroupHeading('LEDs'),
        SettingsRow(
          icon: Icons.grid_on,
          title: 'Per-pixel knockout',
          subtitle: '${state.base.knockout.length} pixel(s) masked',
          onTap: () => context.push(AppRoutes.knockout(lampId)),
        ),
        if (state.lamp.advancedEnabled)
          SettingsRow(
            icon: Icons.memory,
            title: 'Advanced LED setup',
            subtitle:
                'Base ${state.base.px}×${state.base.bpp == 4 ? "RGBW" : "RGB"} · '
                'Shade ${state.shade.px}×${state.shade.bpp == 4 ? "RGBW" : "RGB"}',
            onTap: () => context.push(AppRoutes.advancedLeds(lampId)),
          ),
        if (state.lamp.advancedEnabled) const SettingsGroupHeading('Advanced'),
        if (state.lamp.advancedEnabled)
          SettingsRow(
            icon: Icons.science_outlined,
            title: 'Advanced enabled',
            subtitle: 'Hide to lock advanced settings until next 5-tap unlock',
            trailing: Switch(
              value: true,
              onChanged: (_) => n.setLampAdvancedEnabled(false),
            ),
          ),
      ],
    );
  }
}

/// Lightweight rename modal — keeps the row tappable without taking the
/// user to a full sub-screen for a one-field edit.
Future<void> _showRenameDialog(
  BuildContext context,
  String initial,
  ValueChanged<String> onSave,
) async {
  final ctrl = TextEditingController(text: initial);
  await showDialog<void>(
    context: context,
    builder: (ctx) => AlertDialog(
      backgroundColor: BrandColors.midnightBlack,
      title: const Text('Rename lamp',
          style: TextStyle(color: BrandColors.lampWhite)),
      content: TextField(
        controller: ctrl,
        autofocus: true,
        decoration: const InputDecoration(labelText: 'Name'),
        style: const TextStyle(color: BrandColors.lampWhite),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(ctx).pop(),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: () {
            onSave(ctrl.text.trim());
            Navigator.of(ctx).pop();
          },
          child: const Text('Save'),
        ),
      ],
    ),
  );
}
