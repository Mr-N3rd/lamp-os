import 'dart:async';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/ble_scanner.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../domain/nearby_lamp.dart';

part 'nearby_lamps_notifier.g.dart';

const _staleAfter = Duration(seconds: 30);

/// How often to sweep the roster and prune stale entries. Without this,
/// a lamp that goes silently out of range while no other lamp emits an
/// adv would stick in `state` forever — `_onAd` is the only thing that
/// rebuilds the list, and it only fires when an adv lands.
const _pruneInterval = Duration(seconds: 5);

/// Minimum gap between back-to-back pre-warm requests for the same
/// device id. Scanner emits the same lamp's adv ~16-30 times per
/// second; without a debounce we'd hammer `BleClient.prewarm` 16-30
/// times per second per paired lamp. 10 seconds is comfortably long
/// enough to cover the BLE connect handshake + service discovery
/// without re-triggering until well after the work has completed.
const _prewarmDebounce = Duration(seconds: 10);

/// Maximum rate at which `state` is re-emitted to consumers. A 22-lamp
/// fleet emits ~44 advs/sec; without this throttle every consumer that
/// `ref.watch`es the roster (My Lamps tile list, social screen, picker
/// sheet, ...) would rebuild at 44 Hz. The leading-edge debounce below
/// emits the FIRST change in any window immediately (so a new lamp
/// appears in the UI within ~16 ms of its first adv) and coalesces
/// subsequent changes inside the window into a single trailing emit.
const _stateEmitWindow = Duration(milliseconds: 500);

@Riverpod(keepAlive: true, name: 'bleScannerProvider')
BleScanner bleScanner(Ref ref) => FbpBleScanner();

@Riverpod(keepAlive: true, name: 'nearbyLampsNotifierProvider')
class NearbyLampsNotifier extends _$NearbyLampsNotifier {
  StreamSubscription<BleAdvertisement>? _sub;
  Timer? _pruneTimer;
  // Per-lamp prewarm rate-limit. Keyed by adv `id` (the BLE device id),
  // value is the wall-clock ms of the last prewarm dispatch.
  final Map<String, int> _lastPrewarmMsById = {};
  // Latest computed roster waiting to be flushed to `state`. Null when
  // there's nothing pending. Used by the leading-edge emit throttle.
  List<NearbyLamp>? _pendingRoster;
  // True while the throttle window is "open" — within this window we
  // accumulate into `_pendingRoster` instead of emitting immediately.
  // The trailing-edge timer below flushes whatever's pending when the
  // window closes.
  Timer? _emitWindowTimer;

  @override
  List<NearbyLamp> build() {
    final scanner = ref.read(bleScannerProvider);
    scanner.start();
    _sub = scanner.results().listen(_onAd);
    ref.onDispose(() {
      _sub?.cancel();
      _pruneTimer?.cancel();
      _pruneTimer = null;
      _emitWindowTimer?.cancel();
      _emitWindowTimer = null;
      _pendingRoster = null;
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
      baseRgb: ad.baseRgb,
      shadeRgb: ad.shadeRgb,
      lastSeenEpochMs: now,
      isMesh: ad.isMesh,
    );
    // Build the next-roster against whatever's MOST CURRENT — either the
    // pending list waiting to flush, or the live state if no window is
    // open. Without checking _pendingRoster here, a burst of advs in a
    // single window would each rebuild against stale `state`.
    final base = _pendingRoster ?? state;
    final next = [
      for (final l in base)
        if (l.id != ad.id &&
            now - l.lastSeenEpochMs < _staleAfter.inMilliseconds)
          l,
      updated,
    ];
    _scheduleStateEmit(next);
    // Lazy-start the prune timer on first adv. Widget tests that don't
    // pipe through the scanner stream never create a timer and so don't
    // trip the test framework's `!timersPending` invariant check; in
    // production this fires on the very first lamp adv (well under a
    // second after build()), so the periodic-prune semantics are
    // effectively unchanged.
    _pruneTimer ??= Timer.periodic(_pruneInterval, (_) => _prune());
    _maybePrewarm(ad.id, now);
  }

  /// Leading-edge emit throttle (audit M1). Without this, every adv on
  /// a 22-lamp fleet (~44 Hz) triggered a `state =` assignment which
  /// rebuilt every consumer that wasn't using `.select()`. With this:
  ///   - First adv in any 500 ms window emits immediately (so a new
  ///     lamp shows up in the UI within ~16 ms of its first adv).
  ///   - Subsequent advs in the same window land in `_pendingRoster`.
  ///   - When the window closes, if `_pendingRoster` is non-null and
  ///     differs from the current `state`, flush it. If nothing has
  ///     changed materially, we silently skip the emit.
  void _scheduleStateEmit(List<NearbyLamp> next) {
    if (_emitWindowTimer == null) {
      // Leading edge: emit + open the window.
      state = next;
      _pendingRoster = null;
      _emitWindowTimer = Timer(_stateEmitWindow, _onEmitWindowClose);
    } else {
      // Inside the window: stash the latest computed roster. We don't
      // emit until the window closes.
      _pendingRoster = next;
    }
  }

  void _onEmitWindowClose() {
    _emitWindowTimer = null;
    final pending = _pendingRoster;
    if (pending == null) return;
    _pendingRoster = null;
    // Defensive equality check (audit cq/perf C3): in steady state with
    // a 22-lamp fleet, every adv mutates rssi → every flush emits → every
    // unselect-watching consumer rebuilds. NearbyLamp is @freezed so ==
    // compares all fields. If the trailing-edge pending matches what we
    // already emitted (e.g. _prune ran inside the window but found
    // nothing to drop and _onAd's last update matched the leading-edge
    // emit), skip the redundant state assignment.
    if (_listEquals(pending, state)) return;
    state = pending;
  }

  /// Lightweight content equality for the roster. Lists of @freezed
  /// NearbyLamp; element `==` checks every field.
  static bool _listEquals(List<NearbyLamp> a, List<NearbyLamp> b) {
    if (identical(a, b)) return true;
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }

  /// Pre-warm a GATT connection to a paired lamp as soon as we see its
  /// adv. By the time the user taps the lamp in the UI, the BLE connect
  /// handshake + service discovery are already done — `control_notifier`
  /// short-circuits its own `connect()` call when `isConnected` is
  /// already true. Saves ~300-900 ms off the perceived tap-to-interactive
  /// latency.
  ///
  /// Rate-limited per device id by `_prewarmDebounce`, and the underlying
  /// `BleClient.prewarm` enforces a single-slot mutex across ALL device
  /// ids so we never burn the radio on more than one prewarm at a time.
  void _maybePrewarm(String deviceId, int nowMs) {
    final inv = ref.read(inventoryNotifierProvider).value;
    if (inv == null) return;
    final invNotifier = ref.read(inventoryNotifierProvider.notifier);
    final isPaired = inv.any((l) => l.id == deviceId);
    if (!isPaired) {
      // Cross-platform / iOS-reinstall reconciliation: the new id
      // doesn't match any inventory entry. Check whether the adv's
      // name + colors uniquely identify a paired lamp under an older
      // platform id. If so, the inventory entry's id is updated in
      // place and subsequent advs hit the fast `isPaired = true`
      // branch above.
      final ad = state.firstWhere((l) => l.id == deviceId);
      unawaited(invNotifier.reconcileIdByIdentity(
        newId: deviceId,
        name: ad.name,
        baseRgb: ad.baseRgb,
        shadeRgb: ad.shadeRgb,
      ));
      return;
    }
    // Update inventory "last seen" too — without this, My Lamps says
    // "3 weeks ago" for a paired lamp that the user has been seeing
    // continuously since they opened the app. InventoryNotifier
    // self-caps at one write per 30s per id, so it's safe to call
    // unconditionally on every adv.
    unawaited(invNotifier.touchLastSeen(deviceId));
    // Cache the observed mesh capability so the route resolver can
    // route correctly even when the lamp is out of range. Self-caps
    // at "only writes when the value changes" so this is also safe
    // to call on every adv.
    final advIsMesh = state.firstWhere((l) => l.id == deviceId).isMesh;
    unawaited(invNotifier.rememberMeshState(deviceId, isMesh: advIsMesh));
    final last = _lastPrewarmMsById[deviceId] ?? 0;
    if (nowMs - last < _prewarmDebounce.inMilliseconds) return;
    _lastPrewarmMsById[deviceId] = nowMs;
    final ble = ref.read(bleClientProvider);
    // Fire-and-forget. The BleClient impl guarantees this never throws
    // and handles its own single-slot mutex + connection cleanup on
    // failure.
    unawaited(ble.prewarm(deviceId));
  }

  /// Drop stale entries from `state`. Called both implicitly by
  /// `_onAd` (which filters during its rebuild) and on a 5-second
  /// periodic timer so lamps that disappear silently still get
  /// pruned without waiting for another lamp's adv.
  void _prune() {
    final now = DateTime.now().millisecondsSinceEpoch;
    final base = _pendingRoster ?? state;
    final next = [
      for (final l in base)
        if (now - l.lastSeenEpochMs < _staleAfter.inMilliseconds) l,
    ];
    if (next.length != base.length) {
      _scheduleStateEmit(next);
    }
  }
}
