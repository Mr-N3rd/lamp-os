import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';

void main() {
  test('FakeBleScanner emits the events sent to it', () async {
    final scanner = FakeBleScanner();
    final emitted = <BleAdvertisement>[];
    final sub = scanner.results().listen(emitted.add);
    await scanner.start();
    scanner.emit(const BleAdvertisement(
      id: 'aa',
      name: 'jacko',
      serviceUuids: ['5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      rssi: -55,
    ));
    scanner.emit(const BleAdvertisement(
      id: 'bb',
      name: 'melonie',
      serviceUuids: ['5f64f4c1-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      rssi: -68,
    ));
    await Future<void>.delayed(Duration.zero);
    await sub.cancel();
    expect(emitted.map((a) => a.name).toList(), ['jacko', 'melonie']);
  });
}
