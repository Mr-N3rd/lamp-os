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

  @override
  Future<void> connect(String deviceId) async {
    final device = fbp.BluetoothDevice(
      remoteId: fbp.DeviceIdentifier(deviceId),
    );
    await device.connect(
      license: fbp.License.nonprofit,
      autoConnect: false,
      mtu: 247,
    );
    // Reset the cache on every connect — service handles can change after a
    // reconnect, especially after a firmware reboot that re-registers the
    // GATT database.
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

  @override
  Future<Uint8List> read(String d, String s, String c) async {
    try {
      final ch = await _resolve(d, s, c);
      final bytes = await ch.read();
      return Uint8List.fromList(bytes);
    } on fbp.FlutterBluePlusException catch (e) {
      if (e.toString().toLowerCase().contains('encryption')) {
        throw BleEncryptionRequired(d);
      }
      rethrow;
    }
  }

  @override
  Future<void> write(String d, String s, String c, Uint8List v) async {
    try {
      final ch = await _resolve(d, s, c);
      await ch.write(v, withoutResponse: false);
    } on fbp.FlutterBluePlusException catch (e) {
      if (e.toString().toLowerCase().contains('encryption')) {
        throw BleEncryptionRequired(d);
      }
      rethrow;
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
