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
  }) = _InventoryLamp;

  factory InventoryLamp.fromJson(Map<String, dynamic> json) =>
      _$InventoryLampFromJson(json);
}
