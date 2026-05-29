import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/setup_client.dart';
import 'package:lamp_app/core/ble/uuids.dart';

void main() {
  test('claim writes SSID, password, name, then triggers apply', () async {
    final ble = InMemoryBleClient();
    await ble.connect('dev1');
    final setup = SetupClient(ble: ble);

    await setup.claim(
      deviceId: 'dev1',
      name: 'jacko',
      ssid: 'lamplit-wifi',
      password: 'secret',
    );

    expect(
      utf8.decode(await ble.read('dev1', BleUuids.setupService, BleUuids.setupSsid)),
      'lamplit-wifi',
    );
    expect(
      utf8.decode(await ble.read('dev1', BleUuids.setupService, BleUuids.setupPwd)),
      'secret',
    );
    expect(
      utf8.decode(await ble.read('dev1', BleUuids.setupService, BleUuids.setupName)),
      'jacko',
    );
    expect(
      await ble.read('dev1', BleUuids.setupService, BleUuids.setupApply),
      Uint8List.fromList([0x01]),
    );
  });
}
