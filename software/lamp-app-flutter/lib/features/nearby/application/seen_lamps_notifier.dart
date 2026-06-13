import 'dart:async';
import 'dart:convert';

import 'package:riverpod_annotation/riverpod_annotation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../../../core/ble/ble_scanner.dart';
import '../domain/seen_lamp.dart';
import 'nearby_lamps_notifier.dart';

part 'seen_lamps_notifier.g.dart';

const _prefsKey = 'seenLamps.v1';

/// Hard cap on the seen-list size. 50 entries × ~150 bytes JSON each
/// ≈ 7.5 KB persisted — negligible for SharedPreferences. Older
/// (lastSeen earliest) entries are dropped when a new one bumps the
/// list over.
const _maxEntries = 50;

/// How long to coalesce in-memory updates before persisting to disk.
/// BLE scans fire ~1 Hz per lamp; without a debounce we'd touch
/// SharedPreferences every advertisement.
const _persistDebounce = Duration(seconds: 2);

/// Persistent record of every lamp we've ever heard via BLE adv. Mirrors
/// the same scanner stream `nearbyLampsNotifier` listens to, but keeps
/// entries forever (until evicted by the size cap) so the Info page can
/// show a "Seen before" list alongside the live "Nearby" one.
///
/// State is loaded synchronously from SharedPreferences on first build()
/// and persisted with [_persistDebounce] coalescing on every adv update.
@Riverpod(keepAlive: true, name: 'seenLampsNotifierProvider')
class SeenLampsNotifier extends _$SeenLampsNotifier {
  StreamSubscription<BleAdvertisement>? _sub;
  Timer? _persistTimer;
  bool _ready = false;

  @override
  List<SeenLamp> build() {
    // Subscribe immediately so we don't miss adv events that arrive
    // while prefs are loading. Initial state is empty; the loader
    // merges saved entries in once they're available.
    final scanner = ref.read(bleScannerProvider);
    _sub = scanner.results().listen(_onAd);
    unawaited(_loadFromPrefs());
    ref.onDispose(() {
      _sub?.cancel();
      _persistTimer?.cancel();
    });
    return const [];
  }

  Future<void> _loadFromPrefs() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_prefsKey);
    if (raw == null || raw.isEmpty) {
      _ready = true;
      return;
    }
    try {
      final list = (jsonDecode(raw) as List)
          .cast<Map<String, dynamic>>()
          .map(SeenLamp.fromJson)
          .toList();
      // Merge: anything already in state (from advs that arrived during
      // load) wins over disk. Then re-sort + cap.
      final byId = <String, SeenLamp>{
        for (final l in list) l.id: l,
      };
      for (final l in state) {
        byId[l.id] = l;
      }
      final merged = byId.values.toList()
        ..sort((a, b) => b.lastSeenEpochMs.compareTo(a.lastSeenEpochMs));
      state = merged.take(_maxEntries).toList(growable: false);
    } catch (_) {
      // Corrupt payload — drop it and start fresh. SharedPreferences will
      // be overwritten on the next adv.
    }
    _ready = true;
  }

  void _onAd(BleAdvertisement ad) {
    final now = DateTime.now().millisecondsSinceEpoch;
    final existing = state.indexWhere((l) => l.id == ad.id);
    final entry = existing >= 0
        ? state[existing].copyWith(
            name: ad.name,
            baseRgb: ad.baseRgb,
            shadeRgb: ad.shadeRgb,
            lastSeenEpochMs: now,
          )
        : SeenLamp(
            id: ad.id,
            name: ad.name,
            baseRgb: ad.baseRgb,
            shadeRgb: ad.shadeRgb,
            lastSeenEpochMs: now,
          );
    final next = <SeenLamp>[
      entry,
      for (final l in state)
        if (l.id != ad.id) l,
    ];
    // List is freshly bumped-to-top; remainder stays in prior order which
    // is roughly lastSeen-desc, so a full re-sort isn't needed for the
    // common case. Cap to _maxEntries.
    state = next.length <= _maxEntries
        ? next
        : next.take(_maxEntries).toList(growable: false);
    _schedulePersist();
  }

  void _schedulePersist() {
    if (!_ready) return;
    _persistTimer?.cancel();
    _persistTimer = Timer(_persistDebounce, _persistNow);
  }

  Future<void> _persistNow() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(
      _prefsKey,
      jsonEncode([for (final l in state) l.toJson()]),
    );
  }
}
