import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client.dart';
import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/lamp_crypto.dart';
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
  // Nullable rather than `late final` so a Riverpod retry (which re-runs
  // build() on the same notifier instance after a transient connect/auth
  // failure) can re-assign without throwing LateInitializationError. We
  // dispose the previous writer instances before swapping in new ones.
  WriteCoalescer? _brightnessWriter;
  WriteCoalescer? _shadeColorsWriter;
  WriteCoalescer? _baseColorsWriter;
  // Coalesces slider-drag writes to CHAR_HOME_PREVIEW (cmd 0x02 + value)
  // while the Home Mode page is mounted.
  WriteCoalescer? _homePreviewWriter;

  // Per-pixel knockout debounce: keyed by pixel index.
  final Map<int, Timer?> _knockoutTimers = {};
  final Map<int, int> _knockoutPending = {};

  StreamSubscription<bool>? _connSub;
  Timer? _reconnectTimer;
  static const _reconnectDelays = [500, 1000, 2000, 4000, 8000]; // ms, capped

  // Inventory "last-seen color" debouncer. We don't need to persist every
  // slider tick — only the trailing value. The previous code was awaiting a
  // SharedPreferences write + an inventory-state notification per tick,
  // which dominated frame time during a drag. We collapse all the writes
  // inside a window into one disk write at the trailing edge.
  Timer? _seenFlushTimer;
  LampColor? _pendingSeenShade;
  LampColor? _pendingSeenBase;
  static const _seenFlushDelay = Duration(milliseconds: 500);

  // Snapshot of the state as loaded from the lamp. Used for isDirty checks.
  late ControlState _original;

  // Captured from the build argument. Not `late final` because a Riverpod
  // retry re-runs build() on the same notifier instance — the second pass
  // would reassign and a final field throws LateInitializationError.
  late String _deviceId;

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

  /// Coalesces "last-seen color" updates into one trailing disk write.
  /// Used by `setShadeColor` and `setBaseColors` so a continuous slider
  /// drag at 60 fps doesn't translate into 60 SharedPreferences writes
  /// (and 60 inventory-state notifications that rebuild LampShell).
  void _queueSeen({LampColor? shade, LampColor? base}) {
    if (shade != null) _pendingSeenShade = shade;
    if (base != null) _pendingSeenBase = base;
    _seenFlushTimer?.cancel();
    _seenFlushTimer = Timer(_seenFlushDelay, () => unawaited(_flushSeen()));
  }

  Future<void> _flushSeen() async {
    final s = _pendingSeenShade;
    final b = _pendingSeenBase;
    _pendingSeenShade = null;
    _pendingSeenBase = null;
    if (s == null && b == null) return;
    await _updateSeen(shade: s, base: b);
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
    // Register disconnect now — only after a successful connect. If connect
    // itself threw, there's nothing to tear down, and registering this
    // earlier would crash dispose on platforms where the BLE client can't
    // run (e.g. flutter_blue_plus in unit tests). If auth or section reads
    // throw below, this still fires on container teardown.
    ref.onDispose(() => ble.disconnect(deviceId));
    await AuthClient(ble: ble)
        .authenticate(deviceId: deviceId, password: lamp.controlPassword);
    final fresh = await _readSections(ble);

    // Live-preview writes are fire-and-forget. Swallow errors here so a
    // pending debounce timer that fires after the lamp disconnects (e.g.
    // post-save reboot, or back-navigation tearing down the provider while
    // a timer is in flight) doesn't crash the app. The next reconnect
    // reloads state from the lamp anyway.
    Future<void> safeWrite(String charUuid, Uint8List v) async {
      try {
        await ble.write(deviceId, BleUuids.controlService, charUuid, v);
      } catch (_) {
        // intentionally dropped — live-preview writes are fire-and-forget
      }
    }

    // Dispose any prior writers (from an earlier build() invocation on this
    // instance after a Riverpod retry) before swapping new ones in.
    _brightnessWriter?.dispose();
    _shadeColorsWriter?.dispose();
    _baseColorsWriter?.dispose();
    _homePreviewWriter?.dispose();
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
    _homePreviewWriter = WriteCoalescer(
      onWrite: (v) => safeWrite(BleUuids.homePreview, v),
      debounce: _writeDebounce,
    );

    ref.onDispose(() {
      _brightnessWriter?.dispose();
      _shadeColorsWriter?.dispose();
      _baseColorsWriter?.dispose();
      _homePreviewWriter?.dispose();
      for (final t in _knockoutTimers.values) {
        t?.cancel();
      }
      _knockoutTimers.clear();
      _knockoutPending.clear();
      // Flush any pending "last-seen color" to disk before tearing down so
      // the trailing value of an in-flight drag isn't dropped.
      if (_seenFlushTimer?.isActive == true) {
        _seenFlushTimer!.cancel();
        unawaited(_flushSeen());
      }
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

    await _updateSeen(
      shade: fresh.shade.colors.single,
      base: fresh.base.colors[fresh.base.ac],
    );

    // Self-heal: if a previous session left the firmware's configurator
    // behaviors stuck in disabled=true (the test_expression / complete
    // protocol can leak that state — see ExpressionEditorScreen.dispose),
    // re-enable them on every connect. Cheap one-shot write; safe no-op
    // when the configurators are already enabled.
    await _completeExpressionTest(ble);

    _original = fresh;
    return fresh;
  }

  /// Reads every section characteristic and returns a fresh ControlState.
  /// Assumes the BLE link is connected and authenticated. Used by both
  /// build() (initial load) and save() (post-reboot reload).
  Future<ControlState> _readSections(BleClient ble) async {
    Future<Map<String, dynamic>> readJson(String charUuid) async {
      final bytes =
          await ble.read(_deviceId, BleUuids.controlService, charUuid);
      return jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
    }

    Future<List<dynamic>> readJsonList(String charUuid) async {
      final bytes =
          await ble.read(_deviceId, BleUuids.controlService, charUuid);
      return jsonDecode(utf8.decode(bytes)) as List<dynamic>;
    }

    final lampJson = await readJson(BleUuids.lampSection);
    final baseJson = await readJson(BleUuids.baseSection);
    final shadeJson = await readJson(BleUuids.shadeSection);
    final homeJson = await readJson(BleUuids.homeSection);
    final mqttJson = await readJson(BleUuids.mqttSection);
    final exprList = await readJsonList(BleUuids.exprSection);
    return ControlState(
      lamp: LampSection.fromJson(lampJson),
      base: BaseSection.fromJson(baseJson),
      shade: ShadeSection.fromJson(shadeJson),
      home: HomeSection.fromJson(homeJson),
      mqtt: MqttSection.fromJson(mqttJson),
      expressions: ExpressionsSection.fromJson(exprList),
    );
  }

  // ---------------------------------------------------------------------------
  // Dirty tracking
  // ---------------------------------------------------------------------------

  /// True when the current state differs from the snapshot loaded at build().
  ///
  /// Note: `expressions` is intentionally NOT included. Expressions persist
  /// independently of the settings-blob save flow — `upsertExpression` and
  /// `removeExpression` write to `CHAR_EXPRESSION_OP` which the firmware
  /// persists to NVS immediately. Adding expressions to isDirty would
  /// double-fire Save semantics and surface a Save button on the expressions
  /// tab where there's nothing left to save.
  bool get isDirty {
    final cur = state.value;
    if (cur == null) return false;
    return _isLampDirty(cur.lamp, _original.lamp) ||
        _isBaseDirty(cur.base, _original.base) ||
        _isShadeDirty(cur.shade, _original.shade) ||
        _isHomeDirty(cur.home, _original.home) ||
        _isMqttDirty(cur.mqtt, _original.mqtt);
  }

  bool _isLampDirty(LampSection a, LampSection b) =>
      a.brightness != b.brightness ||
      a.name != b.name ||
      a.advancedEnabled != b.advancedEnabled;

  bool _isBaseDirty(BaseSection a, BaseSection b) =>
      a.ac != b.ac ||
      a.bpp != b.bpp ||
      !_colorsEqual(a.colors, b.colors) ||
      !_knockoutEqual(a.knockout, b.knockout);

  bool _isShadeDirty(ShadeSection a, ShadeSection b) =>
      a.bpp != b.bpp || !_colorsEqual(a.colors, b.colors);

  bool _isHomeDirty(HomeSection a, HomeSection b) =>
      a.ssid != b.ssid ||
      a.password != b.password ||
      a.brightness != b.brightness ||
      a.enabled != b.enabled;

  bool _isMqttDirty(MqttSection a, MqttSection b) =>
      a.enabled != b.enabled ||
      a.brokerHost != b.brokerHost ||
      a.brokerPort != b.brokerPort ||
      a.username != b.username ||
      a.password != b.password ||
      a.topicPrefix != b.topicPrefix;

  bool _colorsEqual(List<LampColor> a, List<LampColor> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }

  bool _knockoutEqual(Map<int, int> a, Map<int, int> b) {
    if (a.length != b.length) return false;
    for (final e in a.entries) {
      if (b[e.key] != e.value) return false;
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

    // Build the full blob from the in-memory ControlState. The firmware
    // dropped read-support for CHAR_SETTINGS_BLOB once the full config grew
    // past 512 bytes (firmware comment in ble_control.cpp:745); per-section
    // characteristics are how we hydrate `cur` at build time, and we have
    // every field we need to round-trip the blob back.
    final blob = <String, dynamic>{
      'lamp': {
        'brightness': cur.lamp.brightness,
        'name': cur.lamp.name,
        'advancedEnabled': cur.lamp.advancedEnabled,
        // lamp.password is set by the setup-service apply path; never
        // round-trip it via settingsBlob. Omitted entirely so the firmware
        // keeps whatever it already has.
      },
      'base': {
        'px': cur.base.px,
        'ac': cur.base.ac,
        'bpp': cur.base.bpp,
        'colors': cur.base.colors.map((c) => c.toHex()).toList(),
        'knockout': [
          for (final e in cur.base.knockout.entries) {'p': e.key, 'b': e.value},
        ],
      },
      'shade': {
        'px': cur.shade.px,
        'bpp': cur.shade.bpp,
        'colors': cur.shade.colors.map((c) => c.toHex()).toList(),
      },
      'homeMode': <String, dynamic>{
        'ssid': cur.home.ssid,
        'brightness': cur.home.brightness,
        'enabled': cur.home.enabled,
        // Only include password if the user actually typed a new one (i.e.
        // it's not the firmware's read-time mask sentinel). Omitting the
        // field lets the firmware keep the existing stored value.
        if (cur.home.password != '********' && cur.home.password.isNotEmpty)
          'password': cur.home.password,
      },
      'mqtt': <String, dynamic>{
        'enabled': cur.mqtt.enabled,
        'brokerHost': cur.mqtt.brokerHost,
        'brokerPort': cur.mqtt.brokerPort,
        'username': cur.mqtt.username,
        'topicPrefix': cur.mqtt.topicPrefix,
        if (cur.mqtt.password != '********' && cur.mqtt.password.isNotEmpty)
          'password': cur.mqtt.password,
      },
      'expressions': [
        for (final e in cur.expressions.expressions) e.toJson(),
      ],
    };

    // 3. Write the merged blob. The firmware will fade out + reboot.
    final blobJson = jsonEncode(blob);
    final inv = await ref.read(inventoryNotifierProvider.future);
    final lamp = inv.firstWhere(
      (l) => l.id == _deviceId,
      orElse: () =>
          throw StateError('lamp $_deviceId not in inventory'),
    );
    final pw = lamp.controlPassword ?? '';
    final payload = pw.isEmpty
        ? Uint8List.fromList([
            LampCrypto.magicPlaintext,
            ...utf8.encode(blobJson),
          ])
        : await LampCrypto.encryptOp(
            op: blob,
            password: pw,
            saltUuid16: uuidSaltLE16(BleUuids.settingsBlob),
            charShortName: 'settingsBlob',
          );
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

    // 4. After the firmware reboots, manually re-read every section and
    //    refresh state. We do NOT call ref.invalidateSelf() — Riverpod would
    //    re-run build() on the same notifier instance and the `late final`
    //    fields above would throw LateInitializationError. Doing the reload
    //    inline keeps the writers + connection subscription intact.
    state = const AsyncLoading<ControlState>();
    Future<void>.delayed(const Duration(seconds: 5), () async {
      if (!ref.mounted) return;
      try {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final lamp = inv.firstWhere(
          (l) => l.id == _deviceId,
          orElse: () =>
              throw StateError('lamp $_deviceId not in inventory'),
        );
        // ble.connect is idempotent — if the reconnect loop already brought
        // the link back up this is a no-op.
        await ble.connect(_deviceId);
        await AuthClient(ble: ble).authenticate(
            deviceId: _deviceId, password: lamp.controlPassword);
        final fresh = await _readSections(ble);
        if (!ref.mounted) return;
        _original = fresh;
        state = AsyncData(fresh);
        await _updateSeen(
          shade: fresh.shade.colors.single,
          base: fresh.base.colors[fresh.base.ac],
        );
      } catch (e, st) {
        if (ref.mounted) state = AsyncError(e, st);
      }
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
    _brightnessWriter?.schedule(Uint8List.fromList([v]));
  }

  Future<void> setShadeColor(LampColor color) async {
    final cur = state.value;
    if (cur == null) return;
    final colors = [color];
    state = AsyncData(cur.copyWith(
      shade: ShadeSection(px: cur.shade.px, bpp: cur.shade.bpp, colors: colors),
    ));
    _shadeColorsWriter?.schedule(_encodeColors(colors));
    _queueSeen(shade: color);
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
        knockout: cur.base.knockout,
      ),
    ));
    _baseColorsWriter?.schedule(_encodeColors(colors));
    if (colors.isNotEmpty) {
      final acIdx = cur.base.ac.clamp(0, colors.length - 1);
      _queueSeen(base: colors[acIdx]);
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
        knockout: cur.base.knockout,
      ),
    ));
    // ac is part of the base settings blob, not its own characteristic; the
    // firmware picks it up on the next CHAR_SETTINGS_BLOB save (Phase 5's
    // Setup screen). Updating locally is enough for the visible session.
  }

  Future<void> setKnockoutPixel(int index, int brightness) async {
    final cur = state.value;
    if (cur == null) return;
    final clamped = brightness.clamp(0, 100);
    final next = Map<int, int>.from(cur.base.knockout);
    if (clamped == 100) {
      next.remove(index); // default — drop the entry to keep the map small
    } else {
      next[index] = clamped;
    }
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: cur.base.px,
        ac: cur.base.ac,
        bpp: cur.base.bpp,
        colors: cur.base.colors,
        knockout: next,
      ),
    ));
    _scheduleKnockoutWrite(index, clamped);
  }

  void _scheduleKnockoutWrite(int index, int brightness) {
    _knockoutPending[index] = brightness;
    _knockoutTimers[index]?.cancel();
    _knockoutTimers[index] = Timer(const Duration(milliseconds: 30), () {
      final v = _knockoutPending.remove(index);
      if (v == null) return;
      final ble = ref.read(bleClientProvider);
      unawaited(_safeWriteKnockout(ble, index, v));
    });
  }

  Future<void> _safeWriteKnockout(
      BleClient ble, int index, int brightness) async {
    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.baseKnockout,
        Uint8List.fromList([index, brightness]),
      );
    } catch (_) {
      // intentionally dropped (same contract as the other live-preview writes)
    }
  }

  /// Reset every knockout entry to 100% in one pass. Cheaper than looping
  /// `setKnockoutPixel` N times because we mutate state once and skip the
  /// per-pixel debounce timers. Firmware has no clear-all sentinel, so we
  /// still write index+100 per previously-edited pixel.
  Future<void> clearKnockout() async {
    final cur = state.value;
    if (cur == null) return;
    final edited = cur.base.knockout.keys.toList();
    if (edited.isEmpty) return;
    for (final t in _knockoutTimers.values) {
      t?.cancel();
    }
    _knockoutTimers.clear();
    _knockoutPending.clear();
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: cur.base.px,
        ac: cur.base.ac,
        bpp: cur.base.bpp,
        colors: cur.base.colors,
        knockout: const {},
      ),
    ));
    final ble = ref.read(bleClientProvider);
    for (final i in edited) {
      unawaited(_safeWriteKnockout(ble, i, 100));
    }
  }

  // ---------------------------------------------------------------------------
  // Setup-screen mutators (state-only; ride the global Save via settingsBlob)
  // ---------------------------------------------------------------------------

  Future<void> setLampName(String name) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      lamp: LampSection(
        name: name,
        brightness: cur.lamp.brightness,
        advancedEnabled: cur.lamp.advancedEnabled,
      ),
    ));
  }

  Future<void> setLampAdvancedEnabled(bool v) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      lamp: LampSection(
        name: cur.lamp.name,
        brightness: cur.lamp.brightness,
        advancedEnabled: v,
      ),
    ));
  }

  Future<void> setHomeSsid(String ssid) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      home: HomeSection(
        ssid: ssid,
        password: cur.home.password,
        brightness: cur.home.brightness,
        enabled: cur.home.enabled,
      ),
    ));
  }

  Future<void> setHomePassword(String password) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      home: HomeSection(
        ssid: cur.home.ssid,
        password: password,
        brightness: cur.home.brightness,
        enabled: cur.home.enabled,
      ),
    ));
  }

  Future<void> setHomeBrightness(int brightness) async {
    final cur = state.value;
    if (cur == null) return;
    final clamped = brightness.clamp(0, 100);
    state = AsyncData(cur.copyWith(
      home: HomeSection(
        ssid: cur.home.ssid,
        password: cur.home.password,
        brightness: clamped,
        enabled: cur.home.enabled,
      ),
    ));
  }

  Future<void> setHomeEnabled(bool enabled) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      home: HomeSection(
        ssid: cur.home.ssid,
        password: cur.home.password,
        brightness: cur.home.brightness,
        enabled: enabled,
      ),
    ));
  }

  // ---------------------------------------------------------------------------
  // Home-mode "preview" — bring the lamp into home mode while the Home Mode
  // setup page is mounted. The firmware brings up the saved Wi-Fi STA so the
  // home brightness slider has live effect; on dispose it drops Wi-Fi again.
  // ---------------------------------------------------------------------------

  /// Tell the firmware to enter home-mode preview: bring up Wi-Fi STA with
  /// the saved home creds (even while BT is connected). Best-effort write
  /// — firmware tolerates a missed enter (the live brightness writes simply
  /// won't have a visible effect until Wi-Fi is up).
  Future<void> enterHomePreview() async {
    final ble = ref.read(bleClientProvider);
    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.homePreview,
        Uint8List.fromList([0x01]),
      );
    } catch (_) {
      // best-effort
    }
  }

  /// Tell the firmware to exit home-mode preview: drop the Wi-Fi STA so
  /// the rest of the BT control session runs without coexistence pressure.
  /// Safe to call after the BT session has already gone away (the write
  /// just no-ops).
  Future<void> exitHomePreview() async {
    final ble = ref.read(bleClientProvider);
    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.homePreview,
        Uint8List.fromList([0x00]),
      );
    } catch (_) {
      // best-effort — BT may already be torn down by page pop
    }
  }

  /// Push a live home-brightness value to the firmware while the Home Mode
  /// page is mounted. The firmware updates `homeMode.brightness` in memory
  /// only; persistence happens via the regular settingsBlob save flow.
  void previewHomeBrightness(int v) {
    final clamped = v.clamp(0, 100);
    _homePreviewWriter?.schedule(Uint8List.fromList([0x02, clamped]));
  }

  Future<void> setMqttEnabled(bool v) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      mqtt: MqttSection(
        enabled: v,
        brokerHost: cur.mqtt.brokerHost,
        brokerPort: cur.mqtt.brokerPort,
        username: cur.mqtt.username,
        password: cur.mqtt.password,
        topicPrefix: cur.mqtt.topicPrefix,
      ),
    ));
  }

  Future<void> setMqttBrokerHost(String host) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      mqtt: MqttSection(
        enabled: cur.mqtt.enabled,
        brokerHost: host,
        brokerPort: cur.mqtt.brokerPort,
        username: cur.mqtt.username,
        password: cur.mqtt.password,
        topicPrefix: cur.mqtt.topicPrefix,
      ),
    ));
  }

  Future<void> setMqttBrokerPort(int port) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      mqtt: MqttSection(
        enabled: cur.mqtt.enabled,
        brokerHost: cur.mqtt.brokerHost,
        brokerPort: port,
        username: cur.mqtt.username,
        password: cur.mqtt.password,
        topicPrefix: cur.mqtt.topicPrefix,
      ),
    ));
  }

  Future<void> setMqttUsername(String username) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      mqtt: MqttSection(
        enabled: cur.mqtt.enabled,
        brokerHost: cur.mqtt.brokerHost,
        brokerPort: cur.mqtt.brokerPort,
        username: username,
        password: cur.mqtt.password,
        topicPrefix: cur.mqtt.topicPrefix,
      ),
    ));
  }

  Future<void> setMqttPassword(String password) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      mqtt: MqttSection(
        enabled: cur.mqtt.enabled,
        brokerHost: cur.mqtt.brokerHost,
        brokerPort: cur.mqtt.brokerPort,
        username: cur.mqtt.username,
        password: password,
        topicPrefix: cur.mqtt.topicPrefix,
      ),
    ));
  }

  Future<void> setMqttTopicPrefix(String topicPrefix) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      mqtt: MqttSection(
        enabled: cur.mqtt.enabled,
        brokerHost: cur.mqtt.brokerHost,
        brokerPort: cur.mqtt.brokerPort,
        username: cur.mqtt.username,
        password: cur.mqtt.password,
        topicPrefix: topicPrefix,
      ),
    ));
  }

  Future<void> setBaseBpp(int bpp) async {
    final cur = state.value;
    if (cur == null) return;
    final clamped = (bpp == 3 || bpp == 4) ? bpp : 4;
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: cur.base.px,
        ac: cur.base.ac,
        bpp: clamped,
        colors: cur.base.colors,
        knockout: cur.base.knockout,
      ),
    ));
  }

  Future<void> setBasePx(int px) async {
    final cur = state.value;
    if (cur == null) return;
    final clamped = px.clamp(1, 255);
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: clamped,
        ac: cur.base.ac,
        bpp: cur.base.bpp,
        colors: cur.base.colors,
        knockout: cur.base.knockout,
      ),
    ));
  }

  Future<void> setShadeBpp(int bpp) async {
    final cur = state.value;
    if (cur == null) return;
    final clamped = (bpp == 3 || bpp == 4) ? bpp : 4;
    state = AsyncData(cur.copyWith(
      shade: ShadeSection(
        px: cur.shade.px,
        bpp: clamped,
        colors: cur.shade.colors,
      ),
    ));
  }

  Future<void> setShadePx(int px) async {
    final cur = state.value;
    if (cur == null) return;
    final clamped = px.clamp(1, 255);
    state = AsyncData(cur.copyWith(
      shade: ShadeSection(
        px: clamped,
        bpp: cur.shade.bpp,
        colors: cur.shade.colors,
      ),
    ));
  }

  // ---------------------------------------------------------------------------
  // Expressions
  // ---------------------------------------------------------------------------

  /// Add or update an expression. The key is the (type, target) tuple, so
  /// upserting twice with the same type+target replaces the entry. Writes the
  /// op to CHAR_EXPRESSION_OP which persists to NVS firmware-side.
  Future<void> upsertExpression(ExpressionConfig entry) async {
    final cur = state.value;
    if (cur == null) return;

    var found = false;
    final next = <ExpressionConfig>[];
    for (final e in cur.expressions.expressions) {
      if (e.type == entry.type && e.target == entry.target) {
        next.add(entry);
        found = true;
      } else {
        next.add(e);
      }
    }
    if (!found) next.add(entry);

    state = AsyncData(cur.copyWith(
      expressions: ExpressionsSection(expressions: next),
    ));
    await _writeExpressionOp({'op': 'upsert', 'entry': entry.toJson()});
  }

  /// Remove the expression keyed by (type, target). No-op if absent.
  Future<void> removeExpression({
    required String type,
    required int target,
  }) async {
    final cur = state.value;
    if (cur == null) return;
    final next = cur.expressions.expressions
        .where((e) => e.type != type || e.target != target)
        .toList();
    state = AsyncData(cur.copyWith(
      expressions: ExpressionsSection(expressions: next),
    ));
    await _writeExpressionOp({
      'op': 'remove',
      'type': type,
      'target': target,
    });
  }

  /// Live-preview an expression configuration without persisting it.
  ///
  /// Sends the firmware-expected envelope `{"a":"test_expression", "type":…,
  /// "target":…}`. Previously this sent the full ExpressionConfig JSON
  /// directly, which the firmware's BLE dispatch couldn't recognize and
  /// silently treated as `test_expression` against a non-existent type —
  /// leaving the configurator behaviors stuck in `disabled=true` until the
  /// next `test_expression_complete` (or lamp reboot). The colors and
  /// parameters are already on the lamp via the `expressionOp` upsert that
  /// preceded this call, so the envelope only needs to name the entry.
  Future<void> testExpression(ExpressionConfig entry) async {
    final ble = ref.read(bleClientProvider);
    final payload = <String, dynamic>{
      'a': 'test_expression',
      'type': entry.type,
      'target': entry.target,
    };
    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.expressionTest,
        Uint8List.fromList(utf8.encode(jsonEncode(payload))),
      );
    } catch (_) {
      // best-effort live write — matches the existing live-preview contract
    }
  }

  /// Tells the firmware to end a `test_expression` preview and re-enable
  /// the configurator behaviors. Safe to call when no preview is in flight
  /// — firmware treats it idempotently. Called from build() (heal stuck
  /// state on connect) and from the expression editor's dispose.
  Future<void> completeExpressionTest() async {
    final ble = ref.read(bleClientProvider);
    await _completeExpressionTest(ble);
  }

  /// Private form so build() can use the BleClient it already holds.
  Future<void> _completeExpressionTest(BleClient ble) async {
    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.expressionTest,
        Uint8List.fromList(
            utf8.encode('{"a":"test_expression_complete"}')),
      );
    } catch (_) {
      // best-effort — same contract as the live-preview writes
    }
  }

  Future<void> _writeExpressionOp(Map<String, dynamic> payload) async {
    final ble = ref.read(bleClientProvider);
    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.expressionOp,
        Uint8List.fromList(utf8.encode(jsonEncode(payload))),
      );
    } catch (_) {
      // best-effort
    }
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
        ?.schedule(Uint8List.fromList([cur.lamp.brightness]));
    _shadeColorsWriter?.schedule(_encodeColors(cur.shade.colors));
    _baseColorsWriter?.schedule(_encodeColors(cur.base.colors));
  }
}
