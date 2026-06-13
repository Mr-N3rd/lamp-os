import 'dart:convert';
import 'dart:typed_data';

import '../../../core/ble/ble_client.dart';
import '../../../core/ble/uuids.dart';

/// Writes [BleUuids.auth] with the lamp's password so subsequent control
/// writes are accepted by the firmware's per-connection auth gate. The
/// firmware allows open access when no password is set (lamp.password is
/// empty), so this is a no-op for [password] == null or "".
class AuthClient {
  AuthClient({required this.ble});

  final BleClient ble;

  Future<void> authenticate({
    required String deviceId,
    required String? password,
  }) async {
    if (password == null || password.isEmpty) return;
    await ble.write(
      deviceId,
      BleUuids.controlService,
      BleUuids.auth,
      Uint8List.fromList(utf8.encode(password)),
    );
  }
}
