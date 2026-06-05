import 'dart:async';
import 'dart:typed_data';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/uuids.dart';
import '../data/wisp_repository.dart';
import '../domain/wisp_status.dart';

part 'wisp_notifier.g.dart';

/// Owns the live [WispStatus] for a single lamp. On build it does one
/// read of `CHAR_WISP_STATUS` and subscribes to its notify stream;
/// thereafter every wispStatus update from the wisp lands in `state`
/// without a round-trip.
///
/// `setZone` / `clearZone` delegate to the repository and rely on the
/// wisp's on-change broadcast (≤ ~2s) to push the updated status back
/// via CHAR_WISP_STATUS. We optimistically reflect the choice in local
/// state so the chip highlight doesn't lag the tap; the notify either
/// confirms or corrects it.
@Riverpod(name: 'wispNotifierProvider')
class WispNotifier extends _$WispNotifier {
  StreamSubscription<Uint8List>? _sub;
  late WispRepository _repo;

  @override
  Future<WispStatus> build(String lampId) async {
    final ble = ref.read(bleClientProvider);
    _repo = WispRepository(ble, lampId);

    ref.onDispose(() {
      _sub?.cancel();
    });

    // Subscribe before the initial read so we never miss a notify that
    // fires between the read returning and the listener attaching.
    _sub = ble
        .subscribe(lampId, BleUuids.controlService, BleUuids.wispStatus)
        .listen((bytes) {
      final next = WispStatus.fromBytes(bytes);
      state = AsyncData(next);
    });

    try {
      return await _repo.readStatus();
    } catch (_) {
      // Read can fail if the BLE link isn't fully ready yet (e.g. the
      // user navigated to the Wisp tab during a reconnect). Empty state
      // is the safe default — the notify subscription will fill it in
      // as soon as the lamp pushes the next status.
      return WispStatus.empty;
    }
  }

  /// Pin the wisp to [zoneId]. Optimistically updates local state to
  /// `zoneSource: appOp` so the UI feels responsive; the next status
  /// notify will replace this with the wisp's authoritative view.
  Future<void> setZone(int zoneId) async {
    final cur = state.value ?? WispStatus.empty;
    state = AsyncData(cur.copyWith(
      currentZone: zoneId,
      zoneSource: 'appOp',
    ));
    try {
      await _repo.setZone(zoneId);
    } catch (_) {
      // Best-effort — if the write fails the next notify (or a manual
      // refresh) will reconcile to the wisp's actual state.
    }
  }

  /// Clear the persisted zone pin. After the wisp processes this, the
  /// next status update will show `zoneSource: firstSeen` (or `none`
  /// if no zone has been observed yet).
  Future<void> clearZone() async {
    final cur = state.value ?? WispStatus.empty;
    state = AsyncData(cur.copyWith(
      // Don't optimistically null currentZone — the wisp may keep
      // following whatever it was on until firstSeen kicks in. Just
      // flip the source so the "Clear selection" button hides.
      zoneSource: 'firstSeen',
    ));
    try {
      await _repo.clearZone();
    } catch (_) {
      // Best-effort.
    }
  }

}
