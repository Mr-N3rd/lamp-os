import 'package:freezed_annotation/freezed_annotation.dart';

part 'nearby_lamp.freezed.dart';
part 'nearby_lamp.g.dart';

/// Firmware defaults from config_types.hpp — used to detect a factory-state
/// lamp from the BLE advertisement alone.
const _defaultName = 'standard';
const _defaultBaseRgb = 0x300783;
const _defaultShadeRgb = 0x000000;

@freezed
abstract class NearbyLamp with _$NearbyLamp {
  const NearbyLamp._();

  const factory NearbyLamp({
    required String id,
    required String name,
    required int rssi,
    required List<String> serviceUuids,
    required int baseRgb,
    required int shadeRgb,
    required int lastSeenEpochMs,
    /// True iff this lamp's firmware advertises the version byte —
    /// i.e. speaks the app's mesh protocol and is fully app-
    /// controllable. Drives the BT-only routing decision in
    /// MyLampsScreen and the `mesh` vs `bluetooth` status dot. v1
    /// firmware (legacy BT-only) and transitional pre-shade-restore
    /// v2 builds both get `false`.
    @Default(false) bool isMesh,
  }) = _NearbyLamp;

  factory NearbyLamp.fromJson(Map<String, dynamic> json) =>
      _$NearbyLampFromJson(json);

  /// True when the advertisement still carries the firmware defaults
  /// (name `standard`, base purple `0x300783`, shade off `0x000000`). The
  /// AddLamp flow uses this to decide between the adopt wizard (factory
  /// default → user must claim and personalize it) and a one-tap add
  /// (anything else → already configured, just add to inventory).
  bool get isFactoryDefault =>
      name == _defaultName &&
      baseRgb == _defaultBaseRgb &&
      shadeRgb == _defaultShadeRgb;
}
