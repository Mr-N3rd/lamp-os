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
}
