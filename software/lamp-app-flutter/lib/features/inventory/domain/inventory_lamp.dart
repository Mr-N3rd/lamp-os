import 'package:freezed_annotation/freezed_annotation.dart';

part 'inventory_lamp.freezed.dart';
part 'inventory_lamp.g.dart';

@freezed
abstract class InventoryLamp with _$InventoryLamp {
  const factory InventoryLamp({
    required String id,
    required String name,
    String? controlPassword,
    /// Persistent random critter pick (1-8) assigned at adopt/add time so
    /// each lamp keeps the same critter friend across sessions and across
    /// the connecting/preview surfaces. Nullable for legacy entries adopted
    /// before this field existed — consumers fall back to a deviceId hash.
    int? critterIndex,
    int? lastSeenEpochMs,
    /// Cached last-seen colors written by `controlNotifier._updateSeen`
    /// on every successful connect-and-read and every settled slider
    /// drag. Persisted via `inventory.v1` in SharedPreferences and read
    /// back by `resolveLampColors` to render My Lamps / picker tiles.
    ///
    /// Shape: `[R, G, B, W]` (4 ints). Legacy entries written before
    /// this field grew the W byte may be `[R, G, B]` (length 3) — the
    /// resolver treats those as `W = 0`, preserving the prior render.
    List<int>? lastShadeColor,
    List<int>? lastBaseColor,
    /// Last observed `isMesh` (capability bit 1 in the adv) — true when
    /// the lamp speaks the app's v0x03 mesh protocol, false for legacy
    /// BT-only firmware. Set by `nearby_lamps_notifier` whenever a fresh
    /// adv arrives. `lampRouteResolver` reads this when the live roster
    /// is empty (lamp out of range) so an offline legacy BT-only lamp
    /// routes to BtOnlyLampScreen instead of stranding the user on
    /// ConnectingView forever. Nullable for legacy inventory entries
    /// written before this field existed — resolvers default to
    /// "assume mesh-capable" for those, mirroring the pre-fix behavior.
    bool? lastKnownIsMesh,
    /// GATT-discovered presence of CHAR_COMMIT (Phase A firmware).
    /// Populated post-connect by `inventory_notifier.updateHasCommitChar`.
    /// Null when not yet probed (pre-existing inventory entries, or the
    /// mid-connect window before the probe resolves). Read sites MUST
    /// default null to false via `?? false` — null means "unknown", and
    /// unknown should fall back to the legacy Save-pill flow rather than
    /// incorrectly enabling Phase A behavior.
    bool? hasCommitChar,
  }) = _InventoryLamp;

  factory InventoryLamp.fromJson(Map<String, dynamic> json) =>
      _$InventoryLampFromJson(json);
}
