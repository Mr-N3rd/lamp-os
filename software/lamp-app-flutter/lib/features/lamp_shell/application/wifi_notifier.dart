import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/uuids.dart';
import '../domain/wifi_state.dart';

part 'wifi_notifier.g.dart';

/// Owns the live `wifiOp` write surface and the `wifiState` notify stream
/// for a single lamp. Seeded by a read on build(); notifies thereafter.
///
/// Per-lamp via the `deviceId` family arg.
@Riverpod(name: 'wifiNotifierProvider')
class WifiNotifier extends _$WifiNotifier {
  StreamSubscription<Uint8List>? _sub;
  late String _deviceId;

  @override
  Future<WifiState> build(String deviceId) async {
    _deviceId = deviceId;
    final ble = ref.read(bleClientProvider);
    _sub = ble
        .subscribe(deviceId, BleUuids.controlService, BleUuids.wifiState)
        .listen(_onNotify);
    ref.onDispose(() => _sub?.cancel());
    final bytes = await ble.read(
        deviceId, BleUuids.controlService, BleUuids.wifiState);
    return _parse(bytes);
  }

  void _onNotify(Uint8List bytes) {
    final cur = state.value ?? const WifiState();
    final next = _parse(bytes);
    // Notifies for state transitions (connecting → connected → failed)
    // typically omit scanResults; preserve the last-known scan list across
    // those transitions so the UI doesn't blink the network list empty.
    state = AsyncData(next.copyWith(
      scanResults:
          next.scanResults.isEmpty ? cur.scanResults : next.scanResults,
    ));
  }

  WifiState _parse(Uint8List bytes) {
    if (bytes.isEmpty) return const WifiState();
    return WifiState.fromJson(
        jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>);
  }

  Future<void> scan() => _writeOp({'op': 'scan'});

  Future<void> connect(String ssid, String password) =>
      _writeOp({'op': 'connect', 'ssid': ssid, 'password': password});

  Future<void> forget() => _writeOp({'op': 'forget'});

  Future<void> _writeOp(Map<String, dynamic> op) async {
    final ble = ref.read(bleClientProvider);
    await ble.write(
      _deviceId,
      BleUuids.controlService,
      BleUuids.wifiOp,
      Uint8List.fromList(utf8.encode(jsonEncode(op))),
    );
  }
}
