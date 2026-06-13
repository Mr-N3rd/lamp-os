import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/encrypted_write.dart';

void main() {
  late InMemoryBleClient ble;
  late int bondCalls;
  late EncryptedWrite ew;

  setUp(() async {
    ble = InMemoryBleClient();
    await ble.connect('d');
    bondCalls = 0;
    ew = EncryptedWrite(
      ble: ble,
      bond: (deviceId) async {
        bondCalls += 1;
      },
    );
  });

  test('first-shot success does not bond', () async {
    await ew.write('d', 's', 'c', Uint8List.fromList([1]));
    expect(bondCalls, 0);
  });

  test('encryption error triggers bond then retry succeeds', () async {
    ble.scheduleEncryptionFailure('d', 's', 'c');
    await ew.write('d', 's', 'c', Uint8List.fromList([42]));
    expect(bondCalls, 1);
    expect(await ble.read('d', 's', 'c'), Uint8List.fromList([42]));
  });

  test('second encryption failure rethrows (no infinite loop)', () async {
    ble.scheduleEncryptionFailure('d', 's', 'c');
    ble.scheduleEncryptionFailure('d', 's', 'c');
    await expectLater(
      ew.write('d', 's', 'c', Uint8List.fromList([1])),
      throwsA(isA<BleEncryptionRequired>()),
    );
    expect(bondCalls, 1);
  });
}
