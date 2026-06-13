import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import '../../control/domain/lamp_color.dart';
import '../../../core/ble/ble_client.dart';
import '../../../core/ble/uuids.dart';
import '../domain/wisp_source_mode.dart';
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

  /// Phase E source mode. Wisp persists in NVS and applies the
  /// appropriate transition (Off → broadcast RESTORE; Manual → push
  /// stored palette into CurrentPalette; Aurora → resume subscription).
  Future<void> setSource(WispSourceMode mode) async {
    await _writeOp({
      'char': 'wispOp',
      'op': 'setSource',
      'mode': wispSourceModeWire(mode),
    });
  }

  /// Phase E manual palette. The wisp persists the (clamped to 10) color
  /// list in NVS and, if currently in Manual mode, pushes it into
  /// CurrentPalette so the lamps repaint without a mode flip. Palette is
  /// emitted as a list of `[r,g,b]` integer triples; W is intentionally
  /// dropped — the lamp's headroom math handles warm tinting locally.
  Future<void> setManualPalette(List<LampColor> palette) async {
    await _writeOp({
      'char': 'wispOp',
      'op': 'setManualPalette',
      'colors': [
        // The wisp dispatcher caps at 10 even if we send more, but we
        // also clamp client-side so a slightly stale UI doesn't waste
        // wire bytes on values that will be silently discarded.
        for (final c in palette.take(10)) [c.r, c.g, c.b],
      ],
    });
  }

  /// Off-mode color the wisp renders on its OWN 30-pixel ring when
  /// sourceMode == Off. Does NOT broadcast paint to the lamp grid —
  /// PaintDistributor stays held off in Off mode, so this color exists
  /// only on the wisp itself, "operating it like a lamp" per the
  /// product UX.
  Future<void> setOffColor(LampColor color) async {
    await _writeOp({
      'char': 'wispOp',
      'op': 'setOffColor',
      'color': <int>[color.r, color.g, color.b],
    });
  }

  /// Push new WiFi credentials to the wisp. The wisp persists them in
  /// NVS and immediately kicks WifiLink to reconnect and StageBeacon to
  /// refresh its BLE advert (so pre-mesh lamps follow the new SSID on
  /// their next scan). The wisp's own connection state surfaces back
  /// through `WispStatus.wifiConnected` on the next status notify.
  ///
  /// SECURITY (accepted threat T1): the WiFi PSK leaks in TWO places —
  /// (a) here on the BLE write to `CHAR_WISP_OP`, and (b) downstream
  /// when the lamp re-broadcasts the wispOp as plaintext MSG_CONTROL_OP
  /// on the ESP-NOW mesh for the wisp to ingest. The mesh-leg leak is
  /// the more concerning one — ESP-NOW range is ~30 m LoS so a sniffer
  /// doesn't have to be visually near the user. The only real fix is
  /// fleet-wide mesh authentication (shared PSK distributed at
  /// provisioning), which was deliberately rejected — see
  /// docs/superpowers/notes/2026-06-10-accepted-security-threats.md.
  /// Threat is bounded by physical proximity at configuration time.
  Future<void> setWifi(String ssid, String password) async {
    await _writeOp({
      'char': 'wispOp',
      'op': 'setWifi',
      'ssid': ssid,
      'pw': password,
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
