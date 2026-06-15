import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart'; // InMemoryBleClient is in this file
import 'package:lamp_app/core/ble/uuids.dart';

void main() {
  group('probeHasCommitChar', () {
    test('returns true when CHAR_COMMIT is in the discovered char list',
        () async {
      final ble = InMemoryBleClient();
      ble.setDiscoveredChars('lamp-a', [BleUuids.commit, BleUuids.brightness]);
      expect(await ble.probeHasCommitChar('lamp-a'), isTrue);
    });

    test('returns false when CHAR_COMMIT is absent', () async {
      final ble = InMemoryBleClient();
      ble.setDiscoveredChars(
          'lamp-b', [BleUuids.brightness, BleUuids.settingsBlob]);
      expect(await ble.probeHasCommitChar('lamp-b'), isFalse);
    });

    test('returns false on unknown device (no discovery)', () async {
      final ble = InMemoryBleClient();
      expect(await ble.probeHasCommitChar('lamp-c'), isFalse);
    });
  });
}
