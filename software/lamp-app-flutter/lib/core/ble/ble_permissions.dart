import 'dart:io' show Platform;

import 'package:permission_handler/permission_handler.dart';

/// Wraps Android runtime BT permissions. iOS handles BT prompts
/// automatically on first scan/connect, so [request]/[isGranted]
/// both no-op true there.
abstract class BlePermissions {
  Future<bool> isGranted();
  Future<bool> request();
}

class AndroidBlePermissions implements BlePermissions {
  static const _perms = [
    Permission.bluetoothScan,
    Permission.bluetoothConnect,
    Permission.locationWhenInUse,
  ];

  @override
  Future<bool> isGranted() async {
    if (!Platform.isAndroid) return true;
    for (final p in _perms) {
      if (!await p.isGranted) return false;
    }
    return true;
  }

  @override
  Future<bool> request() async {
    if (!Platform.isAndroid) return true;
    final results = await _perms.request();
    return results.values.every((s) => s.isGranted);
  }
}
