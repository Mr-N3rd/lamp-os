import 'dart:convert';

import 'package:riverpod_annotation/riverpod_annotation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../domain/inventory_lamp.dart';

part 'inventory_notifier.g.dart';

const _prefsKey = 'inventory.v1';

@Riverpod(keepAlive: true, name: 'inventoryNotifierProvider')
class InventoryNotifier extends _$InventoryNotifier {
  @override
  Future<List<InventoryLamp>> build() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_prefsKey);
    if (raw == null || raw.isEmpty) return const [];
    final decoded = (jsonDecode(raw) as List)
        .cast<Map<String, dynamic>>()
        .map(InventoryLamp.fromJson)
        .toList();
    return decoded;
  }

  Future<void> _persist(List<InventoryLamp> list) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_prefsKey, jsonEncode(list));
    state = AsyncData(list);
  }

  Future<void> add(InventoryLamp lamp) async {
    final current = state.value ?? const [];
    if (current.any((l) => l.id == lamp.id)) return;
    await _persist([...current, lamp]);
  }

  Future<void> remove(String id) async {
    final current = state.value ?? const [];
    await _persist(current.where((l) => l.id != id).toList());
  }

  Future<void> updateSeen(
    String id, {
    List<int>? shade,
    List<int>? base,
  }) async {
    final current = state.value ?? const [];
    final now = DateTime.now().millisecondsSinceEpoch;
    final updated = current.map((l) {
      if (l.id != id) return l;
      return l.copyWith(
        lastSeenEpochMs: now,
        lastShadeColor: shade ?? l.lastShadeColor,
        lastBaseColor: base ?? l.lastBaseColor,
      );
    }).toList();
    await _persist(updated);
  }

  /// Set the auth password the app uses to authenticate with this lamp.
  /// Called by ControlNotifier.setLampPassword right before pushing the
  /// new password to the firmware, so the post-reboot reconnect uses the
  /// new value. Passing null clears it (matches the factory-fresh state).
  Future<void> updatePassword(String id, String? password) async {
    final current = state.value ?? const [];
    final updated = current.map((l) {
      if (l.id != id) return l;
      return l.copyWith(controlPassword: password);
    }).toList();
    await _persist(updated);
  }

  /// Update the cached display name for a lamp. Called by ControlNotifier
  /// after a successful settings_blob save (and on cold-load) so the
  /// `LampPickerSheet` rows reflect the lamp's current name — otherwise the
  /// picker keeps showing the name captured at adopt-time even though the
  /// control screen's "Hello my name is:" header (which reads live state)
  /// shows the updated one. No-op when the id isn't in the inventory.
  Future<void> updateName(String id, String name) async {
    final current = state.value ?? const [];
    final updated = current.map((l) {
      if (l.id != id) return l;
      return l.copyWith(name: name);
    }).toList();
    await _persist(updated);
  }
}
