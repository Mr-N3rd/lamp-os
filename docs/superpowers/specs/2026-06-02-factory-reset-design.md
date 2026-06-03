# Factory Reset

## Context

The lamp keeps everything it knows in one NVS Preferences blob (namespace `lamp`, key `cfg`): name, password, brightness, advancedEnabled, base/shade config, expressions, home mode. There's no way today for the user to wipe it — they're stuck with whatever they configured. The user wants a "Factory Reset" affordance in the Setup tab, gated behind advanced mode and behind a confirmation, that clears the lamp back to its initial "awaiting adoption" state.

Constraint: firmware change is fine. The lamp boots into adoption mode automatically when NVS is empty (Config constructor defaults), so all we need is a way to wipe the blob and reboot.

## Architecture

Reuse the existing settings_blob BLE path. The app writes a partial settings_blob containing a sentinel `{"factoryReset": true}`. The firmware drain detects the sentinel BEFORE running the normal merge: if present, it calls `prefs.clear()` on the lamp namespace and sets `fadeOutRebootRequested = true`. The lamp fades to black over ~500ms and reboots into factory defaults. The app drops the inventory entry for that lamp (since the password is gone too).

This stays in line with the existing "save reboots the lamp" UX pattern. No new BLE characteristic. No firmware-side new entry point. Six-ish lines of firmware code + one new ControlNotifier method + one new Setup row.

## Components

### Firmware

`software/lamp-os/src/lamps/standard_lamp.cpp::settings_blob drain` (around line 773-839): add a sentinel check at the top of the JSON-merge block, before the merge runs.

```cpp
if (incomingDoc["factoryReset"].as<bool>()) {
  prefs.begin("lamp", false);
  prefs.clear();
  prefs.end();
  ble_control::notifyStateChange();
  lamp::fadeOutRebootRequested = true;
  return;  // skip merge — wiping
}
```

That's the entire firmware change.

### App: ControlNotifier method

`lib/features/control/application/control_notifier.dart`: new `factoryReset()` mirroring the structure of `setLampPassword`. Writes `{factoryReset: true}` via settings_blob (encrypted with current password). After the expected reboot-disconnect, removes the lamp from inventory (so the user has to onboard again — accurate, since the lamp is now factory-fresh).

```dart
Future<void> factoryReset() async {
  // build payload → encrypt → BLE write → catch reboot-disconnect
  // → remove from inventory → navigate to lamp picker (handled by UI)
}
```

### App: Setup row + confirmation dialog

`lib/features/lamp_shell/presentation/setup_screen.dart`: add a destructive-styled row at the bottom of the Setup list, gated by `ref.watch(advancedSessionProvider(lampId))`. Tapping it opens a confirmation dialog (no text input — just "Reset" / "Cancel"). The dialog calls `notifier.factoryReset()`.

```dart
if (ref.watch(advancedSessionProvider(lampId)))
  SettingsRow(
    icon: Icons.restore_outlined,
    title: 'Factory reset',
    subtitle: 'Wipe all settings and re-adopt',
    onTap: () => _showFactoryResetDialog(context, lampId, n.factoryReset),
  ),
```

The dialog text reads something like "Reset this lamp to factory defaults? You'll need to onboard it again." Cancel + a red Reset button.

## Behavior after reset

1. User confirms in dialog.
2. App writes `{factoryReset: true}` settings_blob (encrypted with current password — the lamp authenticates the write before clearing).
3. Firmware detects sentinel → `prefs.clear()` → `fadeOutRebootRequested = true`.
4. Lamp fades to black, reboots. Comes up with empty NVS → Config defaults → no password → awaiting adoption.
5. App's BLE write throws the expected disconnect-on-reboot. Catch it, remove the lamp from inventory, navigate to the lamp picker.
6. User onboards the lamp again as if it were fresh.

## Files to modify

- `software/lamp-os/src/lamps/standard_lamp.cpp` — sentinel check in settings_blob drain.
- `software/lamp-app-flutter/lib/features/control/application/control_notifier.dart` — `factoryReset()` method.
- `software/lamp-app-flutter/lib/features/lamp_shell/presentation/setup_screen.dart` — gated SettingsRow + `_showFactoryResetDialog`.

## Verification

- `pio test -e native`, `pio run -e upesy_wroom` — both green.
- `dart analyze`, `flutter test` — both green.
- Hardware: unlock advanced mode, tap Factory Reset, confirm. Lamp fades, reboots. App auto-navigates back to picker. Lamp shows up in onboarding scan as factory-fresh. Re-onboard with a new password. Verify everything works.

## Out of scope

- No multi-device "reset all" affordance — single lamp at a time.
- No undo. Factory reset means factory reset.
- No partial reset ("just expressions" / "just colors"). All-or-nothing.
