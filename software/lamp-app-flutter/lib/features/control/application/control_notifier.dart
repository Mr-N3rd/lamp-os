import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/uuids.dart';
import '../../../core/ble/write_coalescer.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../domain/lamp_color.dart';
import '../domain/sections.dart';
import 'auth_client.dart';
import 'control_state.dart';

part 'control_notifier.g.dart';

const _writeDebounce = Duration(milliseconds: 30);

@Riverpod(keepAlive: false, name: 'controlNotifierProvider')
class ControlNotifier extends _$ControlNotifier {
  late final WriteCoalescer _brightnessWriter;
  late final WriteCoalescer _shadeColorsWriter;
  late final WriteCoalescer _baseColorsWriter;

  StreamSubscription<bool>? _connSub;
  Timer? _reconnectTimer;
  static const _reconnectDelays = [500, 1000, 2000, 4000, 8000]; // ms, capped

  // Snapshot of the state as loaded from the lamp. Used for isDirty checks.
  late ControlState _original;

  // Captured from the build argument for use in save().
  late final String _deviceId;

  // ---------------------------------------------------------------------------
  // Inventory color-cache helpers
  // ---------------------------------------------------------------------------

  List<int> _rgbList(LampColor c) => [c.r, c.g, c.b];

  Future<void> _updateSeen({
    LampColor? shade,
    LampColor? base,
  }) async {
    await ref.read(inventoryNotifierProvider.notifier).updateSeen(
          _deviceId,
          shade: shade == null ? null : _rgbList(shade),
          base: base == null ? null : _rgbList(base),
        );
  }

  @override
  Future<ControlState> build(String deviceId) async {
    _deviceId = deviceId;

    final ble = ref.read(bleClientProvider);
    final inv = await ref.read(inventoryNotifierProvider.future);
    final lamp = inv.firstWhere(
      (l) => l.id == deviceId,
      orElse: () => throw StateError('lamp $deviceId not in inventory'),
    );

    await ble.connect(deviceId);
    // Register disconnect immediately so a failure in auth or section reads
    // still tears down the BLE connection on dispose.
    ref.onDispose(() => ble.disconnect(deviceId));
    await AuthClient(ble: ble)
        .authenticate(deviceId: deviceId, password: lamp.controlPassword);

    Future<Map<String, dynamic>> readJson(String charUuid) async {
      final bytes = await ble.read(deviceId, BleUuids.controlService, charUuid);
      return jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
    }

    final lampJson = await readJson(BleUuids.lampSection);
    final baseJson = await readJson(BleUuids.baseSection);
    final shadeJson = await readJson(BleUuids.shadeSection);

    // Live-preview writes are fire-and-forget. Swallow errors here so a
    // pending debounce timer that fires after the lamp disconnects (e.g.
    // post-save reboot, or back-navigation tearing down the provider while
    // a timer is in flight) doesn't crash the app. The next reconnect
    // reloads state from the lamp anyway.
    Future<void> safeWrite(String charUuid, Uint8List v) async {
      try {
        await ble.write(deviceId, BleUuids.controlService, charUuid, v);
      } catch (_) {
        // intentionally dropped
      }
    }

    _brightnessWriter = WriteCoalescer(
      onWrite: (v) => safeWrite(BleUuids.brightness, v),
      debounce: _writeDebounce,
    );
    _shadeColorsWriter = WriteCoalescer(
      onWrite: (v) => safeWrite(BleUuids.shadeColors, v),
      debounce: _writeDebounce,
    );
    _baseColorsWriter = WriteCoalescer(
      onWrite: (v) => safeWrite(BleUuids.baseColors, v),
      debounce: _writeDebounce,
    );

    ref.onDispose(() {
      _brightnessWriter.dispose();
      _shadeColorsWriter.dispose();
      _baseColorsWriter.dispose();
      // disconnect is handled by the earlier onDispose registered right after
      // connect(), ensuring it always runs even if build() throws mid-way.
    });

    // Subscribe to the connection stream so we can surface unsolicited
    // disconnects and drive the reconnect loop. The first emission will be
    // `true` (we just connected); _onConnectionChange(true) when already
    // connected is a no-op.
    _connSub = ble.watchConnected(deviceId).listen(_onConnectionChange);
    ref.onDispose(() {
      _connSub?.cancel();
      _reconnectTimer?.cancel();
    });

    final loaded = ControlState(
      lamp: LampSection.fromJson(lampJson),
      base: BaseSection.fromJson(baseJson),
      shade: ShadeSection.fromJson(shadeJson),
    );
    await _updateSeen(
      shade: loaded.shade.colors.single,
      base: loaded.base.colors[loaded.base.ac],
    );
    _original = loaded;
    return loaded;
  }

  // ---------------------------------------------------------------------------
  // Dirty tracking
  // ---------------------------------------------------------------------------

  /// True when the current state differs from the snapshot loaded at build().
  bool get isDirty {
    final cur = state.value;
    if (cur == null) return false;
    return _isLampDirty(cur.lamp, _original.lamp) ||
        _isBaseDirty(cur.base, _original.base) ||
        _isShadeDirty(cur.shade, _original.shade);
  }

  bool _isLampDirty(LampSection a, LampSection b) =>
      a.brightness != b.brightness ||
      a.name != b.name ||
      a.advancedEnabled != b.advancedEnabled;

  bool _isBaseDirty(BaseSection a, BaseSection b) =>
      a.ac != b.ac || !_colorsEqual(a.colors, b.colors);

  bool _isShadeDirty(ShadeSection a, ShadeSection b) =>
      !_colorsEqual(a.colors, b.colors);

  bool _colorsEqual(List<LampColor> a, List<LampColor> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }

  // ---------------------------------------------------------------------------
  // Persistence
  // ---------------------------------------------------------------------------

  /// Persists current state to the lamp's NVS by writing CHAR_SETTINGS_BLOB.
  /// The lamp will fade out + reboot in response; the app catches the
  /// expected disconnect and schedules a reconnect by invalidating itself
  /// after a 5-second delay.
  Future<void> save() async {
    final cur = state.value;
    if (cur == null) return;
    if (!cur.connected) return; // wait for reconnect; UI Save action is also disabled

    final ble = ref.read(bleClientProvider);

    // 1. Read the firmware's current full config so we don't blow away fields
    //    we don't manage (expressions, mqtt, homeMode, knockout, etc.).
    final blobBytes = await ble.read(
        _deviceId, BleUuids.controlService, BleUuids.settingsBlob);
    final blob = jsonDecode(utf8.decode(blobBytes)) as Map<String, dynamic>;

    // 2. Merge our local mutations.
    final lampNode =
        (blob['lamp'] as Map<String, dynamic>?) ?? <String, dynamic>{};
    lampNode['brightness'] = cur.lamp.brightness;
    blob['lamp'] = lampNode;

    final baseNode =
        (blob['base'] as Map<String, dynamic>?) ?? <String, dynamic>{};
    baseNode['ac'] = cur.base.ac;
    baseNode['colors'] = cur.base.colors.map((c) => c.toHex()).toList();
    blob['base'] = baseNode;

    final shadeNode =
        (blob['shade'] as Map<String, dynamic>?) ?? <String, dynamic>{};
    shadeNode['colors'] = cur.shade.colors.map((c) => c.toHex()).toList();
    blob['shade'] = shadeNode;

    // 3. Write the merged blob. The firmware will fade out + reboot.
    final payload =
        Uint8List.fromList(utf8.encode(jsonEncode(blob)));
    try {
      await ble.write(
          _deviceId, BleUuids.controlService, BleUuids.settingsBlob, payload);
    } catch (e) {
      // Expected: the reboot disconnects mid-write. Anything else is a real
      // failure; rethrow so the UI can surface it.
      final msg = e.toString().toLowerCase();
      final looksLikeReboot =
          msg.contains('not connected') || msg.contains('disconnect');
      if (!looksLikeReboot) rethrow;
    }

    // 4. Schedule a reconnect by invalidating this provider after a delay.
    //    Riverpod will rebuild and run the standard connect/auth/read path.
    //    Meanwhile the UI shows ConnectingView (loading state).
    state = const AsyncLoading<ControlState>();
    Future<void>.delayed(const Duration(seconds: 5), () {
      if (ref.mounted) ref.invalidateSelf();
    });
  }

  Future<void> setBrightness(int value) async {
    final v = value.clamp(0, 100);
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      lamp: LampSection(
        name: cur.lamp.name,
        brightness: v,
        advancedEnabled: cur.lamp.advancedEnabled,
      ),
    ));
    _brightnessWriter.schedule(Uint8List.fromList([v]));
  }

  Future<void> setShadeColor(LampColor color) async {
    final cur = state.value;
    if (cur == null) return;
    final colors = [color];
    state = AsyncData(cur.copyWith(
      shade: ShadeSection(px: cur.shade.px, bpp: cur.shade.bpp, colors: colors),
    ));
    _shadeColorsWriter.schedule(_encodeColors(colors));
    await _updateSeen(shade: color);
  }

  Future<void> setBaseColors(List<LampColor> colors) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: cur.base.px,
        ac: cur.base.ac.clamp(0, colors.isEmpty ? 0 : colors.length - 1),
        bpp: cur.base.bpp,
        colors: colors,
      ),
    ));
    _baseColorsWriter.schedule(_encodeColors(colors));
    if (colors.isNotEmpty) {
      final acIdx = cur.base.ac.clamp(0, colors.length - 1);
      await _updateSeen(base: colors[acIdx]);
    }
  }

  Future<void> setBaseAc(int index) async {
    final cur = state.value;
    if (cur == null) return;
    final clamped =
        index.clamp(0, cur.base.colors.isEmpty ? 0 : cur.base.colors.length - 1);
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: cur.base.px,
        ac: clamped,
        bpp: cur.base.bpp,
        colors: cur.base.colors,
      ),
    ));
    // ac is part of the base settings blob, not its own characteristic; the
    // firmware picks it up on the next CHAR_SETTINGS_BLOB save (Phase 5's
    // Setup screen). Updating locally is enough for the visible session.
  }

  Uint8List _encodeColors(List<LampColor> colors) {
    final arr = colors.map((c) => c.toHex()).toList();
    return Uint8List.fromList(utf8.encode(jsonEncode(arr)));
  }

  // ---------------------------------------------------------------------------
  // Connection lifecycle
  // ---------------------------------------------------------------------------

  void _onConnectionChange(bool isConnected) {
    final cur = state.value;
    if (cur == null) return;
    if (isConnected && !cur.connected) {
      // Reconnect succeeded — clear flags + push local state to the lamp.
      _reconnectTimer?.cancel();
      state = AsyncData(cur.copyWith(connected: true, reconnectAttempt: 0));
      _pushLocalState(cur);
      return;
    }
    if (!isConnected && cur.connected) {
      state = AsyncData(cur.copyWith(connected: false));
      _scheduleReconnect();
    }
  }

  void _scheduleReconnect() {
    final cur = state.value;
    if (cur == null) return;
    final attempt = cur.reconnectAttempt;
    final delayMs = _reconnectDelays[
        attempt < _reconnectDelays.length ? attempt : _reconnectDelays.length - 1];
    _reconnectTimer?.cancel();
    _reconnectTimer =
        Timer(Duration(milliseconds: delayMs), _tryReconnect);
    state = AsyncData(cur.copyWith(reconnectAttempt: attempt + 1));
  }

  Future<void> _tryReconnect() async {
    final ble = ref.read(bleClientProvider);
    try {
      await ble.connect(_deviceId);
      // Re-auth so subsequent writes get past the firmware's auth gate.
      final inv = await ref.read(inventoryNotifierProvider.future);
      final lamp = inv.firstWhere(
        (l) => l.id == _deviceId,
        orElse: () => throw StateError('lamp $_deviceId not in inventory'),
      );
      await AuthClient(ble: ble)
          .authenticate(deviceId: _deviceId, password: lamp.controlPassword);
      // The watchConnected stream will fire `true` and _onConnectionChange
      // handles clearing the banner + pushing local state.
    } catch (_) {
      // Connect or auth failed — schedule the next attempt.
      _scheduleReconnect();
    }
  }

  void _pushLocalState(ControlState cur) {
    _brightnessWriter
        .schedule(Uint8List.fromList([cur.lamp.brightness]));
    _shadeColorsWriter.schedule(_encodeColors(cur.shade.colors));
    _baseColorsWriter.schedule(_encodeColors(cur.base.colors));
  }
}
