import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/uuids.dart';

part 'dispositions_notifier.g.dart';

/// Per-peer social disposition for a given lamp.
///
/// Reads CHAR_SOCIAL_DISPOSITIONS once at build; subsequent edits update
/// an in-memory map AND schedule a debounced write back to the lamp (500
/// ms after the last edit). The full map is sent on every write — the
/// firmware-side characteristic replaces the entire state on each write.
///
/// Disposition values are 1..5 (salty..neutral..smitten). Missing keys
/// default to 3 (neutral) at the call site via `get`.
@Riverpod(keepAlive: false, name: 'dispositionsProvider')
class Dispositions extends _$Dispositions {
  Timer? _flushTimer;
  Map<String, int> _local = const {};

  @override
  Future<Map<String, int>> build(String lampId) async {
    ref.onDispose(() {
      _flushTimer?.cancel();
    });
    final ble = ref.read(bleClientProvider);
    try {
      final bytes = await ble.read(
        lampId,
        BleUuids.controlService,
        BleUuids.socialDispositions,
      );
      if (bytes.isEmpty) {
        _local = {};
        return _local;
      }
      final parsed = jsonDecode(utf8.decode(bytes));
      if (parsed is Map<String, dynamic>) {
        _local = {
          for (final entry in parsed.entries)
            entry.key: (entry.value as num).toInt().clamp(1, 5),
        };
      } else {
        _local = {};
      }
    } catch (_) {
      // Read can fail before auth; UI treats absence as "all neutral."
      _local = {};
    }
    return _local;
  }

  /// Disposition for `name`, defaulting to 3 (neutral) when unset.
  int get(String name) => _local[name] ?? 3;

  /// Set the disposition for `name`. Updates local state immediately
  /// (so the slider doesn't bounce) and schedules a debounced write
  /// to the lamp 500 ms after the last edit.
  void set(String name, int value) {
    final clamped = value.clamp(1, 5);
    if (name.isEmpty) return;
    final updated = Map<String, int>.from(_local);
    updated[name] = clamped;
    _local = updated;
    state = AsyncData(updated);
    _scheduleFlush();
  }

  void _scheduleFlush() {
    _flushTimer?.cancel();
    _flushTimer = Timer(const Duration(milliseconds: 500), _flush);
  }

  Future<void> _flush() async {
    final ble = ref.read(bleClientProvider);
    final json = jsonEncode(_local);
    try {
      await ble.write(
        lampId,
        BleUuids.controlService,
        BleUuids.socialDispositions,
        Uint8List.fromList(utf8.encode(json)),
      );
    } catch (_) {
      // Best-effort. If the write failed (disconnect, auth lost), the
      // lamp will keep its previous value. UI doesn't surface this to
      // the user — they can re-toggle when reconnected.
    }
  }
}
