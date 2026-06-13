import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/nearby/domain/nearby_lamp.dart';

void main() {
  test('isConfigured is true when controlService is in serviceUuids', () {
    const lamp = NearbyLamp(
      id: 'aa',
      name: 'jacko',
      rssi: -50,
      serviceUuids: ['5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      lastSeenEpochMs: 1,
    );
    expect(lamp.isConfigured, isTrue);
    expect(lamp.isUnconfigured, isFalse);
  });

  test('isUnconfigured is true when only setupService is present', () {
    const lamp = NearbyLamp(
      id: 'bb',
      name: '',
      rssi: -70,
      serviceUuids: ['5f64f4c1-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      lastSeenEpochMs: 1,
    );
    expect(lamp.isUnconfigured, isTrue);
    expect(lamp.isConfigured, isFalse);
  });
}
