import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/auth_client.dart';

void main() {
  test('writes CHAR_AUTH with the utf8-encoded password', () async {
    final ble = InMemoryBleClient();
    await ble.connect('dev1');
    final auth = AuthClient(ble: ble);

    await auth.authenticate(deviceId: 'dev1', password: 'open sesame');

    final written = await ble.read(
        'dev1', BleUuids.controlService, BleUuids.auth);
    expect(utf8.decode(written), 'open sesame');
  });

  test('no-op when password is null or empty', () async {
    final ble = InMemoryBleClient();
    await ble.connect('dev1');
    final auth = AuthClient(ble: ble);

    await auth.authenticate(deviceId: 'dev1', password: null);
    await auth.authenticate(deviceId: 'dev1', password: '');

    expect(
      () => ble.read('dev1', BleUuids.controlService, BleUuids.auth),
      throwsA(isA<BleNotFound>()),
    );
  });
}
