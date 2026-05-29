import 'dart:convert';
import 'dart:typed_data';

import 'ble_client.dart';
import 'uuids.dart';

class SetupClient {
  SetupClient({required this.ble});

  final BleClient ble;

  Future<void> claim({
    required String deviceId,
    required String name,
    required String ssid,
    required String password,
  }) async {
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupSsid,
        Uint8List.fromList(utf8.encode(ssid)));
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupPwd,
        Uint8List.fromList(utf8.encode(password)));
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupName,
        Uint8List.fromList(utf8.encode(name)));
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupApply,
        Uint8List.fromList([0x01]));
  }
}
