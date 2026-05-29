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
    List<int>? lastShadeColor,
    List<int>? lastBaseColor,
  }) = _InventoryLamp;

  factory InventoryLamp.fromJson(Map<String, dynamic> json) =>
      _$InventoryLampFromJson(json);
}
