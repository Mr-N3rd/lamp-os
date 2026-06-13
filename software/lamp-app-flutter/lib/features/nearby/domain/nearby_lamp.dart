import 'package:freezed_annotation/freezed_annotation.dart';

import '../../../core/ble/uuids.dart';

part 'nearby_lamp.freezed.dart';
part 'nearby_lamp.g.dart';

@freezed
abstract class NearbyLamp with _$NearbyLamp {
  const NearbyLamp._();

  const factory NearbyLamp({
    required String id,
    required String name,
    required int rssi,
    required List<String> serviceUuids,
    required int lastSeenEpochMs,
  }) = _NearbyLamp;

  factory NearbyLamp.fromJson(Map<String, dynamic> json) =>
      _$NearbyLampFromJson(json);

  bool get isConfigured => serviceUuids
      .map((u) => u.toLowerCase())
      .contains(BleUuids.controlService);

  bool get isUnconfigured => !isConfigured && serviceUuids
      .map((u) => u.toLowerCase())
      .contains(BleUuids.setupService);
}
