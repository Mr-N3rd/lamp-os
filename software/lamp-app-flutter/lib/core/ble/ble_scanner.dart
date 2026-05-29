import 'dart:async';

import 'package:flutter_blue_plus/flutter_blue_plus.dart' as fbp;

import 'uuids.dart';

class BleAdvertisement {
  const BleAdvertisement({
    required this.id,
    required this.name,
    required this.serviceUuids,
    required this.rssi,
  });

  final String id;
  final String name;
  final List<String> serviceUuids;
  final int rssi;
}

abstract class BleScanner {
  /// Stream of scan results. Implementations must filter to lamp services
  /// (control + setup) so callers don't see unrelated BLE advertisements.
  Stream<BleAdvertisement> results();

  /// Begin scanning. Must be called once before [results] yields anything
  /// on the real driver. Idempotent — calling twice is a no-op.
  Future<void> start();

  /// Stop scanning (frees the radio).
  Future<void> stop();
}

class FbpBleScanner implements BleScanner {
  StreamSubscription<List<fbp.ScanResult>>? _sub;
  final _ctrl = StreamController<BleAdvertisement>.broadcast();
  bool _running = false;

  @override
  Stream<BleAdvertisement> results() => _ctrl.stream;

  @override
  Future<void> start() async {
    if (_running) return;
    _running = true;
    _sub = fbp.FlutterBluePlus.scanResults.listen((results) {
      for (final r in results) {
        final uuids = r.advertisementData.serviceUuids
            .map((g) => g.str128.toLowerCase())
            .toList();
        const wanted = {BleUuids.controlService, BleUuids.setupService};
        if (!uuids.any(wanted.contains)) continue;
        _ctrl.add(BleAdvertisement(
          id: r.device.remoteId.str,
          name: r.advertisementData.advName.isNotEmpty
              ? r.advertisementData.advName
              : r.device.platformName,
          serviceUuids: uuids,
          rssi: r.rssi,
        ));
      }
    });
    // No withServices filter — Android can miss matches when the service
    // UUID is in the scan response rather than the main advertisement.
    // The callback above already filters to our two service UUIDs.
    await fbp.FlutterBluePlus.startScan(
      timeout: const Duration(minutes: 5),
      continuousUpdates: true,
    );
  }

  @override
  Future<void> stop() async {
    if (!_running) return;
    _running = false;
    await fbp.FlutterBluePlus.stopScan();
    await _sub?.cancel();
    _sub = null;
  }
}

class FakeBleScanner implements BleScanner {
  final _ctrl = StreamController<BleAdvertisement>.broadcast();
  bool _started = false;

  @override
  Stream<BleAdvertisement> results() => _ctrl.stream;

  @override
  Future<void> start() async {
    _started = true;
  }

  @override
  Future<void> stop() async {
    _started = false;
  }

  void emit(BleAdvertisement ad) {
    if (!_started) {
      throw StateError('scanner not started');
    }
    _ctrl.add(ad);
  }
}
