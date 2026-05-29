import 'dart:typed_data';

import 'ble_client.dart';

typedef BondFn = Future<void> Function(String deviceId);

class EncryptedWrite {
  EncryptedWrite({required this.ble, required this.bond});

  final BleClient ble;
  final BondFn bond;

  Future<void> write(
    String deviceId,
    String serviceUuid,
    String charUuid,
    Uint8List value,
  ) async {
    try {
      await ble.write(deviceId, serviceUuid, charUuid, value);
    } on BleEncryptionRequired {
      await bond(deviceId);
      await ble.write(deviceId, serviceUuid, charUuid, value);
      // If the second write also fails with BleEncryptionRequired,
      // it will propagate naturally (no catch for second attempt)
    }
  }
}
