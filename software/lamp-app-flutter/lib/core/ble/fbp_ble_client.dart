import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart' as fbp;

import 'ble_client.dart';

/// Production [BleClient] backed by flutter_blue_plus. Each public method
/// resolves the device + service + characteristic from the plugin's live
/// registry, errors out with a friendly exception type when it can't.
class FbpBleClient implements BleClient {
  /// Cache of discovered GATT services per device id. Populated lazily on the
  /// first `read`/`write`/`subscribe` after a successful connect. Without this
  /// every BLE write would re-run a full service discovery on the radio,
  /// which is what tipped the lamp into LINK_SUPERVISION_TIMEOUT during rapid
  /// slider drags. Cleared on disconnect so a reconnect gets a fresh discovery.
  final Map<String, List<fbp.BluetoothService>> _serviceCache = {};

  /// Single-slot mutex for pre-warm. Pre-warming holds the lamp's BLE
  /// peripheral and consumes some of its mesh airtime even at WIDE
  /// conn-params, so we cap concurrent pre-warms at one — the latest
  /// caller waits behind the in-flight one or no-ops if it's already
  /// connected by then.
  Future<void>? _prewarmInFlight;

  @override
  Future<void> prewarm(String deviceId) async {
    if (isConnected(deviceId)) return;
    final inFlight = _prewarmInFlight;
    if (inFlight != null) {
      // Another pre-warm is in progress. Drop this request — the
      // pre-warm payoff is "have a hot connection ready when the user
      // taps"; if the in-flight pre-warm targets a different lamp, the
      // user is no worse off than today.
      return;
    }
    _prewarmInFlight = (() async {
      try {
        await connect(deviceId);
        // Force service discovery now so the cache is hot before the
        // user taps. _resolve() does this lazily on first I/O — we just
        // do it eagerly here.
        final device = fbp.BluetoothDevice(
          remoteId: fbp.DeviceIdentifier(deviceId),
        );
        _serviceCache[deviceId] = await device.discoverServices();
      } catch (_) {
        // Best-effort — never let a pre-warm failure surface. If the
        // user does end up tapping this lamp, the normal connect path
        // will re-try and report any real failure.
        try {
          await disconnect(deviceId);
        } catch (_) {}
      } finally {
        _prewarmInFlight = null;
      }
    })();
    await _prewarmInFlight;
  }

  @override
  Future<void> connect(String deviceId) async {
    final device = fbp.BluetoothDevice(
      remoteId: fbp.DeviceIdentifier(deviceId),
    );
    // NOTE on scan/connect coex (audit L2): we do NOT explicitly pause
    // the background scan here. flutter_blue_plus serializes scan and
    // connect operations internally on both Android (BluetoothLeScanner
    // pause during gatt.connect) and iOS (CoreBluetooth's
    // centralManager will defer scan callbacks during a connect-in-
    // progress). If we ever see GATT MTU exchange failures correlated
    // with continuous scanning (the cited iOS edge case), revisit by
    // calling `FlutterBluePlus.stopScan()` here + a hook to re-issue
    // start in NearbyLampsNotifier after `disconnect()`. Until then,
    // the extra coordination cost (passing scanner refs into the BLE
    // client) isn't worth the imagined gain.
    //
    // Android's BLE stack throws status=133 ("generic GATT error") on the
    // first connect attempt fairly often — stale GATT cache from a prior
    // session, peer slot still releasing on the lamp side, or just bad RF
    // luck. It almost always works on the second try. One retry with a
    // short backoff swallows that flake without bubbling it to the UI; if
    // the second attempt also throws, let it propagate so we don't mask a
    // real failure (lamp powered off, out of range, etc.).
    try {
      await device.connect(
        license: fbp.License.nonprofit,
        autoConnect: false,
        mtu: 247,
      );
    } on fbp.FlutterBluePlusException {
      await Future<void>.delayed(const Duration(milliseconds: 600));
      await device.connect(
        license: fbp.License.nonprofit,
        autoConnect: false,
        mtu: 247,
      );
    }
    // Android caches the lamp's GATT service definitions per-device for
    // unbonded peers. After a firmware re-flash (which re-registers
    // services with new handles), discoverServices() will silently
    // return the STALE cached set, and every subsequent read/write
    // misses → the phone gives up and tears the link down with reason
    // 531 (BLE_ERR_REM_USER_CONN_TERM). clearGattCache wraps the
    // hidden BluetoothGatt.refresh() call and invalidates that cache
    // so the next discoverServices() does a real GATT exchange.
    // Android-only (iOS no-op via thrown FbpErrorCode.androidOnly).
    try {
      await device.clearGattCache();
    } catch (_) {
      // best-effort — iOS throws androidOnly, some Android versions
      // reject without a connected GATT; neither case should fail the
      // whole connect.
    }
    // Ask Android for a tighter connection interval (11.25-15ms vs the
    // default ~49ms) — wraps BluetoothGatt.requestConnectionPriority(
    // CONNECTION_PRIORITY_HIGH). Without this, slider-rate live writes
    // ceiling at ~11Hz (one write per pair of connection events) even
    // with WRITE_NR and a rate-paced WriteCoalescer. HIGH costs the
    // phone some battery while connected, but the lamp is line-powered
    // and the user is actively interacting — fair trade. iOS ignores;
    // some Androids decline; either way swallow the error.
    try {
      await device.requestConnectionPriority(
        connectionPriorityRequest: fbp.ConnectionPriority.high,
      );
    } catch (_) {
      // best-effort — not all platforms honor this
    }
    // Reset our in-app cache too — service handles can change after a
    // reconnect, especially after a firmware reboot that re-registers
    // the GATT database. clearGattCache above invalidates Android's
    // system cache; this clears ours so the next _resolve() does a
    // fresh discoverServices().
    _serviceCache.remove(deviceId);
  }

  @override
  Future<void> disconnect(String deviceId) async {
    final device = fbp.BluetoothDevice(
      remoteId: fbp.DeviceIdentifier(deviceId),
    );
    _serviceCache.remove(deviceId);
    await device.disconnect();
  }

  @override
  bool isConnected(String deviceId) {
    return fbp.FlutterBluePlus.connectedDevices.any(
      (d) => d.remoteId.str == deviceId,
    );
  }

  Future<fbp.BluetoothCharacteristic> _resolve(
    String deviceId,
    String serviceUuid,
    String charUuid,
  ) async {
    final device = fbp.FlutterBluePlus.connectedDevices
        .firstWhere(
          (d) => d.remoteId.str == deviceId,
          orElse: () => throw BleNotFound('device $deviceId not connected'),
        );

    // Reuse the cached service list when present — discoverServices() is a
    // multi-round-trip GATT exchange and re-running it per write floods the
    // link. Only call it on a cold cache (first I/O after connect).
    final services = _serviceCache[deviceId] ??=
        await device.discoverServices();

    final service = services.firstWhere(
      (s) => s.uuid.str128.toLowerCase() == serviceUuid.toLowerCase(),
      orElse: () => throw BleNotFound('service $serviceUuid on $deviceId'),
    );
    final ch = service.characteristics.firstWhere(
      (c) => c.uuid.str128.toLowerCase() == charUuid.toLowerCase(),
      orElse: () => throw BleNotFound('char $charUuid in $serviceUuid'),
    );
    return ch;
  }

  /// Inspect an fbp exception and rethrow it as one of our typed
  /// exceptions when the shape is unambiguous. Centralised so the
  /// string-match for fbp's reworded error messages lives in exactly
  /// ONE place at the platform boundary (audit cq-H / W7.8).
  Never _classifyAndRethrow(String deviceId, Object e) {
    final msg = e.toString().toLowerCase();
    if (msg.contains('encryption')) {
      throw BleEncryptionRequired(deviceId);
    }
    if (msg.contains('disconnect') || msg.contains('not connected')) {
      throw BleDisconnectedException(deviceId, e);
    }
    throw e;
  }

  @override
  Future<Uint8List> read(String d, String s, String c) async {
    try {
      final ch = await _resolve(d, s, c);
      final bytes = await ch.read();
      // Size cap (audit sec-M3) — refuses payloads > kBleMaxReadBytes
      // before they reach any jsonDecode path. Protects the app from a
      // hostile lamp or a runaway firmware notification that would
      // otherwise burn unbounded memory.
      if (bytes.length > kBleMaxReadBytes) {
        throw BleReadTooLarge(d, bytes.length, kBleMaxReadBytes);
      }
      return Uint8List.fromList(bytes);
    } on fbp.FlutterBluePlusException catch (e) {
      _classifyAndRethrow(d, e);
    }
  }

  @override
  Future<void> write(
    String d,
    String s,
    String c,
    Uint8List v, {
    bool withoutResponse = false,
    bool allowLongWrite = false,
  }) async {
    try {
      final ch = await _resolve(d, s, c);
      await ch.write(
        v,
        withoutResponse: withoutResponse,
        allowLongWrite: allowLongWrite,
      );
    } on fbp.FlutterBluePlusException catch (e) {
      _classifyAndRethrow(d, e);
    }
  }

  @override
  Stream<Uint8List> subscribe(String d, String s, String c) async* {
    final ch = await _resolve(d, s, c);
    await ch.setNotifyValue(true);
    yield* ch.lastValueStream.map(Uint8List.fromList);
  }

  @override
  Stream<bool> watchConnected(String deviceId) {
    final device = fbp.BluetoothDevice(
      remoteId: fbp.DeviceIdentifier(deviceId),
    );
    return device.connectionState
        .map((s) => s == fbp.BluetoothConnectionState.connected);
  }
}
