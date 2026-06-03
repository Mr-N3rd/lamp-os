import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/settings_row.dart';
import '../../control/application/advanced_session.dart';
import '../../control/application/control_notifier.dart';
import '../../control/application/control_state.dart';
import '../../control/presentation/widgets/connecting_view.dart';

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
    final hasSsid = state.home.ssid.isNotEmpty;
    final enabled = state.home.enabled;
    final String homeSubtitle;
    if (enabled) {
      homeSubtitle = hasSsid
          ? '${state.home.ssid} · ${state.home.brightness}%'
          : 'On · not configured';
    } else {
      homeSubtitle = hasSsid ? 'Off · ${state.home.ssid} saved' : 'Off';
    }
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
        SettingsRow(
          icon: Icons.lock_outline,
          title: 'Password',
          subtitle: 'Tap to change',
          onTap: () => _showPasswordDialog(context, lampId, n.setLampPassword),
        ),
        const SettingsGroupHeading('Connectivity'),
        SettingsRow(
          icon: Icons.home_outlined,
          title: 'Home Mode',
          subtitle: homeSubtitle,
          trailing: Switch(
            value: enabled,
            // Soft toggle: flips the enabled flag without wiping the saved
            // SSID/password. Destructive "Forget network" lives inside the
            // Home Mode pane. First-time on (no SSID yet): drill into the
            // pane so the user can pick a network.
            onChanged: (v) {
              n.setHomeEnabled(v);
              if (v && !hasSsid) {
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
        // Advanced LED setup is gated on the session-only advanced flag —
        // user must have tapped 5× on the Info wordmark this connection.
        // Flag resets on BLE disconnect (see ControlNotifier._onConnectionChange).
        if (ref.watch(advancedSessionProvider(lampId)))
          SettingsRow(
            icon: Icons.memory,
            title: 'Advanced LED setup',
            subtitle:
                'Base ${state.base.px}×${state.base.byteOrder} · '
                'Shade ${state.shade.px}×${state.shade.byteOrder}',
            onTap: () => context.push(AppRoutes.advancedLeds(lampId)),
          ),
        // Factory reset — also gated on session-advanced. Destructive:
        // wipes NVS and re-adopts. Dialog confirms before firing.
        if (ref.watch(advancedSessionProvider(lampId)))
          SettingsRow(
            icon: Icons.restore_outlined,
            title: 'Factory reset',
            subtitle: 'Wipe all settings and re-adopt',
            onTap: () => _showFactoryResetDialog(context, n.factoryReset),
          ),
      ],
    );
  }
}

/// Lightweight rename modal — keeps the row tappable without taking the
/// user to a full sub-screen for a one-field edit. Hosted by a
/// `StatefulWidget` so the `TextEditingController` has a lifecycle to
/// dispose on (a plain `showDialog` closure leaks the controller).
Future<void> _showRenameDialog(
  BuildContext context,
  String initial,
  ValueChanged<String> onSave,
) =>
    showDialog<void>(
      context: context,
      builder: (_) => _RenameDialog(initial: initial, onSave: onSave),
    );

class _RenameDialog extends StatefulWidget {
  const _RenameDialog({required this.initial, required this.onSave});
  final String initial;
  final ValueChanged<String> onSave;

  @override
  State<_RenameDialog> createState() => _RenameDialogState();
}

class _RenameDialogState extends State<_RenameDialog> {
  late final TextEditingController _ctrl =
      TextEditingController(text: widget.initial);

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: BrandColors.midnightBlack,
      title: const Text('Rename lamp',
          style: TextStyle(color: BrandColors.lampWhite)),
      content: TextField(
        controller: _ctrl,
        autofocus: true,
        decoration: const InputDecoration(labelText: 'Name'),
        style: const TextStyle(color: BrandColors.lampWhite),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: () {
            widget.onSave(_ctrl.text.trim());
            Navigator.of(context).pop();
          },
          child: const Text('Save'),
        ),
      ],
    );
  }
}

/// Password change modal — same shape as the rename dialog, but the field
/// is obscured by default with an eye-toggle reveal. No old-password
/// confirmation: the user is already authenticated for this BLE session,
/// that's the gate. Empty passwords are rejected by the firmware path the
/// same way they would be at onboarding, so we guard for non-empty here.
Future<void> _showPasswordDialog(
  BuildContext context,
  String lampId,
  Future<void> Function(String) onSave,
) =>
    showDialog<void>(
      context: context,
      builder: (_) => _PasswordDialog(onSave: onSave),
    );

class _PasswordDialog extends StatefulWidget {
  const _PasswordDialog({required this.onSave});
  final Future<void> Function(String) onSave;

  @override
  State<_PasswordDialog> createState() => _PasswordDialogState();
}

class _PasswordDialogState extends State<_PasswordDialog> {
  final _ctrl = TextEditingController();
  bool _obscured = true;

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      backgroundColor: BrandColors.midnightBlack,
      title: const Text('Change password',
          style: TextStyle(color: BrandColors.lampWhite)),
      content: TextField(
        controller: _ctrl,
        autofocus: true,
        obscureText: _obscured,
        decoration: InputDecoration(
          labelText: 'New password',
          suffixIcon: IconButton(
            icon: Icon(_obscured ? Icons.visibility : Icons.visibility_off),
            onPressed: () => setState(() => _obscured = !_obscured),
          ),
        ),
        style: const TextStyle(color: BrandColors.lampWhite),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: () {
            final pw = _ctrl.text;
            if (pw.isEmpty) return;
            // Fire-and-forget — setLampPassword drives lampSaveStatus, so
            // the user sees the "Saving changes…" overlay while the lamp
            // reboots and reconnects.
            widget.onSave(pw);
            Navigator.of(context).pop();
          },
          child: const Text('Save'),
        ),
      ],
    );
  }
}

/// Factory-reset confirmation. Destructive operation, so we go through a
/// straightforward Cancel/Reset dialog (no text confirmation step like
/// some apps require — the advanced-mode gesture barrier and the
/// confirm-tap are gate enough). On Reset, the notifier sends the
/// settings_blob sentinel and the lamp reboots into factory defaults.
/// We pop the dialog before the BLE write returns so the UI doesn't
/// hang on the reboot-disconnect.
Future<void> _showFactoryResetDialog(
  BuildContext context,
  Future<void> Function() onReset,
) =>
    showDialog<void>(
      context: context,
      builder: (_) => AlertDialog(
        backgroundColor: BrandColors.midnightBlack,
        title: const Text('Factory reset?',
            style: TextStyle(color: BrandColors.lampWhite)),
        content: const Text(
          "This wipes all settings on this lamp and returns it to its "
          "out-of-box state. You'll need to onboard it again.",
          style: TextStyle(color: BrandColors.fogGrey),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(),
            child: const Text('Cancel'),
          ),
          FilledButton(
            style: FilledButton.styleFrom(
              backgroundColor: BrandColors.error,
            ),
            onPressed: () {
              onReset();
              Navigator.of(context).pop();
            },
            child: const Text('Reset'),
          ),
        ],
      ),
    );
