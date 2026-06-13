import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_scanner.dart';
import 'package:lamp_app/features/nearby/application/nearby_lamps_notifier.dart';

void main() {
  test('emits scan advertisements and dedupes by id', () async {
    final scanner = FakeBleScanner();
    final container = ProviderContainer(
      overrides: [bleScannerProvider.overrideWithValue(scanner)],
    );
    addTearDown(container.dispose);

    // Subscribe so the notifier starts.
    final sub =
        container.listen(nearbyLampsNotifierProvider, (_, _) {});
    addTearDown(sub.close);

    await Future<void>.delayed(const Duration(milliseconds: 10));
    scanner.emit(const BleAdvertisement(
      id: 'aa',
      name: 'jacko',
      serviceUuids: ['5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      baseRgb: 0x300783,
      shadeRgb: 0x000000,
      rssi: -55,
    ));
    await Future<void>.delayed(const Duration(milliseconds: 10));
    expect(container.read(nearbyLampsNotifierProvider).length, 1);

    // Same id — should replace, not duplicate.
    scanner.emit(const BleAdvertisement(
      id: 'aa',
      name: 'jacko',
      serviceUuids: ['5f64f4d0-d6d9-4a44-9b3f-3a8d6f7e6b40'],
      baseRgb: 0x300783,
      shadeRgb: 0x000000,
      rssi: -50,
    ));
    // 600 ms > the 500 ms leading-edge emit window in
    // NearbyLampsNotifier — without this, the second adv lands as
    // pending and the trailing-edge flush hasn't happened yet by the
    // time the test reads state. (M1 audit fix: state emissions are
    // throttled to once per 500 ms so a 22-lamp fleet doesn't
    // re-notify every consumer at 44 Hz.)
    await Future<void>.delayed(const Duration(milliseconds: 600));
    final lamps = container.read(nearbyLampsNotifierProvider);
    expect(lamps.length, 1);
    expect(lamps.first.rssi, -50);
  });
}
