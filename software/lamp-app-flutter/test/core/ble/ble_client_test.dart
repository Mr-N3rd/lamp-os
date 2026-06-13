import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';

void main() {
  late InMemoryBleClient ble;

  setUp(() => ble = InMemoryBleClient());

  test('write then read returns the same bytes', () async {
    await ble.connect('dev1');
    await ble.write('dev1', 'svc', 'chr', Uint8List.fromList([1, 2, 3]));
    final read = await ble.read('dev1', 'svc', 'chr');
    expect(read, Uint8List.fromList([1, 2, 3]));
  });

  test('reading a missing characteristic throws BleNotFound', () async {
    await ble.connect('dev1');
    expect(
      () => ble.read('dev1', 'svc', 'missing'),
      throwsA(isA<BleNotFound>()),
    );
  });

  test('subscribe streams subsequent writes', () async {
    await ble.connect('dev1');
    final events = <Uint8List>[];
    final sub = ble.subscribe('dev1', 'svc', 'chr').listen(events.add);
    await ble.write('dev1', 'svc', 'chr', Uint8List.fromList([7]));
    await ble.write('dev1', 'svc', 'chr', Uint8List.fromList([8]));
    await Future<void>.delayed(Duration.zero);
    await sub.cancel();
    expect(events.map((b) => b.first).toList(), [7, 8]);
  });

  test('scheduleEncryptionFailure throws insufficientEncryption once', () async {
    await ble.connect('dev1');
    ble.scheduleEncryptionFailure('dev1', 'svc', 'enc');
    expect(
      () => ble.write('dev1', 'svc', 'enc', Uint8List.fromList([0])),
      throwsA(isA<BleEncryptionRequired>()),
    );
    // Second write succeeds.
    await ble.write('dev1', 'svc', 'enc', Uint8List.fromList([1]));
    expect(await ble.read('dev1', 'svc', 'enc'), Uint8List.fromList([1]));
  });

  test('InMemoryBleClient tracks connect/disconnect', () async {
    final ble = InMemoryBleClient();
    expect(ble.isConnected('dev1'), isFalse);
    await ble.connect('dev1');
    expect(ble.isConnected('dev1'), isTrue);
    await ble.disconnect('dev1');
    expect(ble.isConnected('dev1'), isFalse);
  });

  test('write throws BleNotConnected on a disconnected device', () async {
    final ble = InMemoryBleClient();
    expect(
      () => ble.write('dev1', 'svc', 'chr', Uint8List.fromList([1])),
      throwsA(isA<BleNotConnected>()),
    );
  });

  test('write succeeds after connect', () async {
    final ble = InMemoryBleClient();
    await ble.connect('dev1');
    await ble.write('dev1', 'svc', 'chr', Uint8List.fromList([1]));
    expect(await ble.read('dev1', 'svc', 'chr'), Uint8List.fromList([1]));
  });
}
