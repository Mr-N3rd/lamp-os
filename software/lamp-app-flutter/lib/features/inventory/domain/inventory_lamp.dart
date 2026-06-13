import 'package:freezed_annotation/freezed_annotation.dart';

part 'inventory_lamp.freezed.dart';
part 'inventory_lamp.g.dart';

@freezed
abstract class InventoryLamp with _$InventoryLamp {
  const factory InventoryLamp({
    required String id,
    required String name,
    int? lastSeenEpochMs,
    List<int>? lastShadeColor,
    List<int>? lastBaseColor,
  }) = _InventoryLamp;

  factory InventoryLamp.fromJson(Map<String, dynamic> json) =>
      _$InventoryLampFromJson(json);
}
