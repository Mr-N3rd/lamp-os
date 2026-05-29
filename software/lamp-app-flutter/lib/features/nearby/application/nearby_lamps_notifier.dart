import 'dart:async';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_scanner.dart';
import '../domain/nearby_lamp.dart';

part 'nearby_lamps_notifier.g.dart';

const _staleAfter = Duration(seconds: 30);

@Riverpod(keepAlive: true, name: 'bleScannerProvider')
BleScanner bleScanner(Ref ref) => FbpBleScanner();

@Riverpod(keepAlive: true, name: 'nearbyLampsNotifierProvider')
class NearbyLampsNotifier extends _$NearbyLampsNotifier {
  StreamSubscription<BleAdvertisement>? _sub;

  @override
  List<NearbyLamp> build() {
    final scanner = ref.read(bleScannerProvider);
    scanner.start();
    _sub = scanner.results().listen(_onAd);
    ref.onDispose(() {
      _sub?.cancel();
      scanner.stop();
    });
    return const [];
  }

  void _onAd(BleAdvertisement ad) {
    final now = DateTime.now().millisecondsSinceEpoch;
    final updated = NearbyLamp(
      id: ad.id,
      name: ad.name,
      rssi: ad.rssi,
      serviceUuids: ad.serviceUuids,
      lastSeenEpochMs: now,
    );
    final next = [
      for (final l in state)
        if (l.id != ad.id &&
            now - l.lastSeenEpochMs < _staleAfter.inMilliseconds)
          l,
      updated,
    ];
    state = next;
  }
}
