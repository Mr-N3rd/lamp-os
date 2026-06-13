import 'dart:convert';
import 'dart:typed_data';

import 'ble_client.dart';
import 'uuids.dart';

class SetupClient {
  SetupClient({required this.ble});

  final BleClient ble;

  /// Writes the lamp's own password (`lamp.password` → gates the BLE control
  /// surface) and its display name, then triggers apply (reboot). The setup
  /// service's SSID characteristic is intentionally not written — v1 firmware
  /// accepts and discards it. Home-mode WiFi is configured later, in the
  /// per-lamp Setup screen.
  Future<void> claim({
    required String deviceId,
    required String name,
    required String password,
  }) async {
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupPwd,
        Uint8List.fromList(utf8.encode(password)));
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupName,
        Uint8List.fromList(utf8.encode(name)));
    await ble.write(deviceId, BleUuids.setupService, BleUuids.setupApply,
        Uint8List.fromList([0x01]));
  }
}
