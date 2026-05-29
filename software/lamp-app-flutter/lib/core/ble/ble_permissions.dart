import 'dart:io' show Platform;

import 'package:permission_handler/permission_handler.dart';

/// Wraps Android runtime BT permissions. iOS handles BT prompts
/// automatically on first scan/connect, so [request]/[isGranted]
/// both no-op true there.
abstract class BlePermissions {
  Future<bool> isGranted();
  Future<bool> request();

  /// True if Android has marked the permission "Don't ask again". From here,
  /// the system prompt no longer appears — the user has to grant from app
  /// settings. UI should call [openSettings] in that case.
  Future<bool> isPermanentlyDenied();

  /// Open the OS app-settings screen so the user can flip the BT toggle
  /// after they've previously denied permanently.
  Future<void> openSettings();
}

class AndroidBlePermissions implements BlePermissions {
  // Android 12+ (API 31+) uses BLUETOOTH_SCAN with `neverForLocation` and
  // doesn't need location. Older Android isn't a supported target here.
  static const _perms = [
    Permission.bluetoothScan,
    Permission.bluetoothConnect,
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

  @override
  Future<bool> isPermanentlyDenied() async {
    if (!Platform.isAndroid) return false;
    for (final p in _perms) {
      if (await p.isPermanentlyDenied) return true;
    }
    return false;
  }

  @override
  Future<void> openSettings() => openAppSettings();
}
