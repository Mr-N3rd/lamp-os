import 'dart:async';

import 'package:flutter_blue_plus/flutter_blue_plus.dart' as fbp;

/// BLE Manufacturer ID the firmware advertises (0xA455). The 6 bytes after
/// the company-ID prefix carry base RGB + shade RGB. See firmware:
/// software/lamp-os/src/components/network/bluetooth.cpp:83-93.
const _lampMfgId = 0xA455;

class BleAdvertisement {
  const BleAdvertisement({
    required this.id,
    required this.name,
    required this.serviceUuids,
    required this.baseRgb,
    required this.shadeRgb,
    required this.rssi,
    this.isMesh = false,
  });

  final String id;
  final String name;
  final List<String> serviceUuids;
  /// Base color in 0xRRGGBB form, parsed from the lamp manufacturer data.
  final int baseRgb;
  /// Shade color in 0xRRGGBB form, parsed from the lamp manufacturer data.
  /// `0` for legacy 6-byte-payload v2 firmware (no shade in adv).
  final int shadeRgb;
  final int rssi;

  /// True iff this lamp's firmware advertises the version byte
  /// (mfg.length >= 7 && mfg[6] >= 2) — i.e. it speaks the app's
  /// mesh protocol and is fully app-controllable. v1 lamps and
  /// transitional pre-shade-restore v2 builds get `false` (the
  /// former because they're genuinely BT-only, the latter because
  /// they won't be on the network long).
  final bool isMesh;
}

abstract class BleScanner {
  /// Stream of scan results. Implementations must filter to lamp
  /// advertisements (manufacturer-data magic 0xA455) so callers don't see
  /// unrelated BLE traffic.
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
        // Firmware only puts the lamp magic + colors in manufacturer data;
        // 128-bit service UUIDs are NOT in the advertisement (would overflow
        // the 31-byte adv limit — see firmware ble_control.cpp:640-644).
        final mfg = r.advertisementData.manufacturerData[_lampMfgId];
        if (mfg == null || mfg.length < 3) continue;
        // The 2-byte company ID prefix (= lamp magic 0xA455) is what
        // fbp keys by; mfg here is the bytes AFTER that. Three shapes
        // in the wild:
        //   - 4 bytes [bR,bG,bB,meshFlag]: transitional v2 build that
        //     dropped shade. Real shade only available via cache /
        //     section read; adv shadeRgb defaults to 0.
        //   - 6 bytes [bR,bG,bB,sR,sG,sB]: v1 firmware (legacy). Has
        //     real base + shade in adv.
        //   - 7 bytes [bR,bG,bB,sR,sG,sB,version]: current firmware
        //     after shade-restore. Version byte at index 6 (>=2 means
        //     "supports the app's mesh protocol").
        final hasShade = mfg.length >= 6;
        final isMesh = mfg.length >= 7 && mfg[6] >= 2;
        _ctrl.add(BleAdvertisement(
          id: r.device.remoteId.str,
          name: r.advertisementData.advName.isNotEmpty
              ? r.advertisementData.advName
              : r.device.platformName,
          serviceUuids: r.advertisementData.serviceUuids
              .map((g) => g.str128.toLowerCase())
              .toList(),
          baseRgb: (mfg[0] << 16) | (mfg[1] << 8) | mfg[2],
          shadeRgb: hasShade
              ? (mfg[3] << 16) | (mfg[4] << 8) | mfg[5]
              : 0,
          rssi: r.rssi,
          isMesh: isMesh,
        ));
      }
    });
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
