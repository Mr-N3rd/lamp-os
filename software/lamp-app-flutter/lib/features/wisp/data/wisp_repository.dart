import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import '../../../core/ble/ble_client.dart';
import '../../../core/ble/uuids.dart';
import '../domain/wisp_status.dart';

/// Thin wrapper around the two Phase-D wisp BLE characteristics. The
/// notifier layer owns lifetime/state; this class is just IO.
///
/// `CHAR_WISP_OP` accepts plaintext JSON (the WriteRouter on the lamp
/// is plaintext-flavored — see `software/lamp-os/src/components/network/
/// ble_control.cpp`, search `CHAR_WISP_OP`). Auth is still gated by the
/// connection-level `isAuthed` check, which the BLE session already
/// satisfies by the time the user reaches the Wisp tab.
class WispRepository {
  WispRepository(this._ble, this._deviceId);

  final BleClient _ble;
  final String _deviceId;

  /// One-shot read of the merged wispStatus JSON. Empty / `"{}"` /
  /// unparseable payloads map to [WispStatus.empty].
  Future<WispStatus> readStatus() async {
    final bytes = await _ble.read(
      _deviceId,
      BleUuids.controlService,
      BleUuids.wispStatus,
    );
    return WispStatus.fromBytes(bytes);
  }

  /// Stream of wispStatus updates. The lamp pushes a notify whenever
  /// `cacheWispStatus()` ingests a fresh CONTROL_OP from the wisp, so
  /// changes from `setZone`/`clearZone` typically round-trip within
  /// ~2s (the wisp's on-change broadcast cadence).
  Stream<WispStatus> watchStatus() {
    return _ble
        .subscribe(
          _deviceId,
          BleUuids.controlService,
          BleUuids.wispStatus,
        )
        .map(WispStatus.fromBytes);
  }

  /// Pin the wisp to [zoneId]. Persisted in wisp NVS — survives reboot.
  Future<void> setZone(int zoneId) async {
    await _writeOp({
      'char': 'wispOp',
      'op': 'setZone',
      'zoneId': zoneId,
    });
  }

  /// Revert the wisp to first-seen-wins. Clears the NVS pin.
  Future<void> clearZone() async {
    await _writeOp({
      'char': 'wispOp',
      'op': 'clearZone',
    });
  }

  Future<void> _writeOp(Map<String, dynamic> payload) async {
    await _ble.write(
      _deviceId,
      BleUuids.controlService,
      BleUuids.wispOp,
      Uint8List.fromList(utf8.encode(jsonEncode(payload))),
    );
  }
}
