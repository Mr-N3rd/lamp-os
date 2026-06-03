import 'package:riverpod_annotation/riverpod_annotation.dart';

part 'advanced_session.g.dart';

/// Session-only "advanced mode" flag per lamp. Holds whether the user has
/// unlocked advanced UI in the current connection session.
///
/// Distinct from the firmware-persisted `LampSettings.advancedEnabled`
/// which the lamp itself stores in NVS — this provider is purely
/// app-side state, scoped to the current BLE connection session, and
/// resets to `false` whenever that session ends (handled by
/// `ControlNotifier._onConnectionChange(false)`).
///
/// Gates visibility of advanced UI like the expression cascade controls
/// and the Setup Hub's Advanced LED setup row. The actual feature state
/// (cascade params, LED config) lives in the lamp config and persists
/// across sessions independently of this flag — this only controls
/// whether the controls are visible.
@Riverpod(keepAlive: true, name: 'advancedSessionProvider')
class AdvancedSession extends _$AdvancedSession {
  @override
  bool build(String lampId) => false;

  void enable() => state = true;
  void disable() => state = false;
  void toggle() => state = !state;
}
