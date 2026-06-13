import '../../../core/widgets/status_dot.dart';
import '../../nearby/domain/nearby_lamp.dart';

/// Derives a [StatusKind] for a lamp at the moment we're looking at it.
///
/// - `mesh` — the active connection right now. Pulses; the app is actually
///   reading + writing this lamp.
/// - `bluetooth` — heard via BLE adv within the nearby staleness window but
///   not the one we're connected to (or connection is currently dropped).
/// - `offline` — not heard at all.
///
/// Pure function so both `LampChip` in the AppBar and the rows in
/// `LampPickerSheet` share the same logic and stay in unit-test reach.
StatusKind statusFor({
  required String lampId,
  required List<NearbyLamp> nearby,
  required bool connected,
}) {
  if (connected) return StatusKind.mesh;
  final inNearby = nearby.any((l) => l.id == lampId);
  return inNearby ? StatusKind.bluetooth : StatusKind.offline;
}
