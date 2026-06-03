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
import '../../social/domain/social_mode.dart';
import 'advanced_session.dart';
import 'auth_client.dart';
import 'control_state.dart';
import 'lamp_auth_required_exception.dart';
import 'lamp_save_status.dart';

part 'control_notifier.g.dart';

/// Marker error type set by ControlNotifier.factoryReset so callers can
/// distinguish "user reset this lamp" from genuine BLE / parse failures.
class FactoryResetSentinel implements Exception {
  const FactoryResetSentinel();
  @override
  String toString() => 'lamp was factory-reset';
}

// Live-preview write debounce. WriteCoalescer fires on a timer, not
// on completion of the previous write — so this value has to stay
// at or below the slowest link the producer might face. Tried 30ms
// (paired with HIGH connection priority + WRITE_NR): drag felt
// smoother but Android happily queued writes the radio/lamp couldn't
// drain fast enough — visible as a post-release tail where the lamp
// kept changing colors after the user stopped dragging. 60ms (~17Hz
// per channel) stays under the sustained drain rate on a HIGH-priority
// link without queue buildup. A proper backpressure refactor of
// WriteCoalescer (one-in-flight, coalesce-to-latest) would let us
// drop this safely; until then 60ms is the floor.
const _writeDebounce = Duration(milliseconds: 60);

/// Fallback used when `shade.colors` comes back empty from the lamp.
/// Firmware enforces exactly one shade color today, but a corrupted NVS
/// payload (or a future firmware change) could yield 0 or 2+ — defensive
/// fallback avoids a `StateError` from `.single` blowing up the connect /
/// post-save reconnect paths.
const _blackShade = LampColor(r: 0, g: 0, b: 0, w: 0);

/// Disables Riverpod's framework-level auto-retry for the control notifier.
/// We run our own reconnect loop with explicit backoff (`_reconnectDelays`)
/// and two concurrent framework retries observed racing the scheduler when
/// multiple lamps failed connect at once. Returning null keeps the provider
/// in error state until an explicit `ref.invalidate` or a new build().
Duration? _noRetry(int retryCount, Object error) => null;

@Riverpod(keepAlive: false, name: 'controlNotifierProvider', retry: _noRetry)
class ControlNotifier extends _$ControlNotifier {
  // Nullable for defensive re-assignability. With framework retry disabled
  // (see `_noRetry`) build() is no longer re-entered on the same instance
  // after a connect/auth failure, but an explicit `ref.invalidate` could
  // still trigger rebuild; nullable shape costs nothing and removes the
  // LateInitializationError class of bug entirely.
  WriteCoalescer? _brightnessWriter;
  WriteCoalescer? _shadeColorsWriter;
  WriteCoalescer? _baseColorsWriter;

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

  // Cached provider references captured in build(). Timer / writer
  // callbacks fire asynchronously and could land AFTER ref.onDispose
  // has run; calling `ref.read(...)` then throws. The underlying
  // BleClient + InventoryNotifier instances both live in keepAlive
  // providers, so a cached pointer stays valid for the lifetime of
  // those providers (the whole app) regardless of this notifier's
  // own dispose timing.
  late BleClient _ble;
  late InventoryNotifier _inv;

  // ---------------------------------------------------------------------------
  // Inventory color-cache helpers
  // ---------------------------------------------------------------------------

  /// RGBW list shape persisted in inventory (and read back by
  /// `resolveLampColors`). Includes the warm-white byte so warm-heavy
  /// edits render correctly on the My Lamps / picker tiles — the BLE
  /// adv carries only RGB triplets, so the cache is the only source
  /// of W for offline / between-edit rendering.
  List<int> _rgbwList(LampColor c) => [c.r, c.g, c.b, c.w];

  Future<void> _updateSeen({
    LampColor? shade,
    LampColor? base,
  }) async {
    // Use cached notifier ref so the trailing-flush path
    // (`_seenFlushTimer` + the `ref.onDispose` final flush) can't
    // touch a disposed Riverpod ref.
    await _inv.updateSeen(
      _deviceId,
      shade: shade == null ? null : _rgbwList(shade),
      base: base == null ? null : _rgbwList(base),
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

    // Cache provider refs once. Used by Timer / coalescer callbacks
    // that can fire after this notifier's onDispose has run.
    _ble = ref.read(bleClientProvider);
    _inv = ref.read(inventoryNotifierProvider.notifier);
    final ble = _ble;
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

    // Auth-gate canary. Post-commit 71415e0 the firmware returns empty bytes
    // from lampSection on unauthenticated reads (so an unauth'd peer can't
    // exfiltrate the password embedded in the section blob). If our stored
    // credential is missing or stale, surface that as a typed sentinel the
    // UI can catch and convert into a password prompt — otherwise the empty
    // bytes propagate into jsonDecode and surface as a generic FormatException.
    final canaryBytes = await ble.read(
        deviceId, BleUuids.controlService, BleUuids.lampSection);
    if (canaryBytes.isEmpty) {
      throw const LampAuthRequiredException();
    }

    final fresh = await _readSections(ble);

    // Live-preview writes are fire-and-forget. Swallow errors here so a
    // pending debounce timer that fires after the lamp disconnects (e.g.
    // post-save reboot, or back-navigation tearing down the provider while
    // a timer is in flight) doesn't crash the app. The next reconnect
    // reloads state from the lamp anyway.
    Future<void> safeWrite(String charUuid, Uint8List v) async {
      try {
        // withoutResponse: true — these are slider-rate live-preview
        // writes. Write-with-response forces a per-write GATT ACK round
        // trip, which at the typical ~49ms connection interval caps
        // throughput at ~5 writes/sec (observed: drains landing at
        // 195ms intervals = ~5Hz). Write-without-response lets the
        // radio fire writes at its raw rate (~33Hz with the 30ms
        // coalescer debounce). Errors are still caught + dropped — a
        // failed live-preview write should not crash the UI.
        await ble.write(
          deviceId,
          BleUuids.controlService,
          charUuid,
          v,
          withoutResponse: true,
        );
      } catch (_) {
        // intentionally dropped — live-preview writes are fire-and-forget
      }
    }

    // Dispose any prior writers (from an earlier build() invocation on this
    // instance after a Riverpod retry) before swapping new ones in.
    _brightnessWriter?.dispose();
    _shadeColorsWriter?.dispose();
    _baseColorsWriter?.dispose();
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
      _brightnessWriter?.dispose();
      _shadeColorsWriter?.dispose();
      _baseColorsWriter?.dispose();
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
      shade: fresh.shade.colors.isEmpty
          ? _blackShade
          : fresh.shade.colors.first,
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
    final exprList = await readJsonList(BleUuids.exprSection);
    return ControlState(
      lamp: LampSection.fromJson(lampJson),
      base: BaseSection.fromJson(baseJson),
      shade: ShadeSection.fromJson(shadeJson),
      home: HomeSection.fromJson(homeJson),
      expressions: ExpressionsSection.fromJson(exprList),
    );
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
        _isShadeDirty(cur.shade, _original.shade) ||
        _isHomeDirty(cur.home, _original.home) ||
        _isExpressionsDirty(
            cur.expressions.expressions, _original.expressions.expressions);
  }

  /// Diagnostic: returns the name of the first section that's dirty, or
  /// `null` when everything's clean. Useful for tracking down false-positive
  /// `isDirty` reports (e.g. "Save changes pill appeared on a fresh
  /// connect" — call this from a logging hook to see WHICH section drifted).
  /// Cheap; same comparisons as `isDirty`, just labelled.
  String? get dirtyReason {
    final cur = state.value;
    if (cur == null) return null;
    if (_isLampDirty(cur.lamp, _original.lamp)) return 'lamp';
    if (_isBaseDirty(cur.base, _original.base)) return 'base';
    if (_isShadeDirty(cur.shade, _original.shade)) return 'shade';
    if (_isHomeDirty(cur.home, _original.home)) return 'home';
    if (_isExpressionsDirty(
        cur.expressions.expressions, _original.expressions.expressions)) {
      return 'expressions';
    }
    return null;
  }

  bool _isLampDirty(LampSection a, LampSection b) =>
      a.brightness != b.brightness ||
      a.name != b.name ||
      a.advancedEnabled != b.advancedEnabled ||
      a.socialMode != b.socialMode;

  bool _isBaseDirty(BaseSection a, BaseSection b) =>
      a.ac != b.ac ||
      a.bpp != b.bpp ||
      a.byteOrder != b.byteOrder ||
      a.px != b.px ||
      !_colorsEqual(a.colors, b.colors) ||
      !_knockoutEqual(a.knockout, b.knockout);

  bool _isShadeDirty(ShadeSection a, ShadeSection b) =>
      a.bpp != b.bpp ||
      a.byteOrder != b.byteOrder ||
      a.px != b.px ||
      !_colorsEqual(a.colors, b.colors);

  bool _isHomeDirty(HomeSection a, HomeSection b) =>
      a.ssid != b.ssid ||
      a.brightness != b.brightness ||
      a.enabled != b.enabled;

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

  bool _isExpressionsDirty(
      List<ExpressionConfig> a, List<ExpressionConfig> b) {
    if (a.length != b.length) return true;
    for (var i = 0; i < a.length; i++) {
      if (!_expressionEqual(a[i], b[i])) return true;
    }
    return false;
  }

  bool _expressionEqual(ExpressionConfig a, ExpressionConfig b) {
    if (a.type != b.type ||
        a.enabled != b.enabled ||
        a.target != b.target ||
        a.intervalMin != b.intervalMin ||
        a.intervalMax != b.intervalMax) {
      return false;
    }
    if (!_colorsEqual(a.colors, b.colors)) return false;
    if (a.parameters.length != b.parameters.length) return false;
    for (final e in a.parameters.entries) {
      if (b.parameters[e.key] != e.value) return false;
    }
    return true;
  }


  // ---------------------------------------------------------------------------
  // Persistence
  // ---------------------------------------------------------------------------

  /// Persists current state to the lamp's NVS by writing CHAR_SETTINGS_BLOB.
  /// The settings_blob payload includes the full `expressions[]` array, so
  /// expression edits ride this same path — no separate `expressionOp` flush
  /// is needed at save time (those writes already updated firmware in-memory
  /// during live preview). The lamp fades out + reboots in response; the app
  /// catches the expected disconnect and reconnects after a 5-second delay.
  Future<void> save() async {
    final cur = state.value;
    if (cur == null) return;
    if (!cur.connected) return; // wait for reconnect; UI Save action is also disabled

    final ble = ref.read(bleClientProvider);

    // Build a PARTIAL blob — only the sections that have changed since
    // the last load/save. The firmware merges incoming top-level keys
    // with the existing NVS config, so anything we omit is preserved.
    // This is essential to keep the encrypted payload under fbp's
    // 512-byte ATT limit; the full blob with several expressions can
    // easily hit 600+ bytes.
    final blob = <String, dynamic>{};
    if (_isLampDirty(cur.lamp, _original.lamp)) {
      blob['lamp'] = {
        'brightness': cur.lamp.brightness,
        'name': cur.lamp.name,
        'advancedEnabled': cur.lamp.advancedEnabled,
        'socialMode': cur.lamp.socialMode.wire,
        // lamp.password is set by the setup-service apply path; never
        // round-trip it via settingsBlob.
      };
    }
    if (_isBaseDirty(cur.base, _original.base)) {
      blob['base'] = {
        'px': cur.base.px,
        'ac': cur.base.ac,
        'bpp': cur.base.bpp,
        'byteOrder': cur.base.byteOrder,
        'colors': cur.base.colors.map((c) => c.toHex()).toList(),
        // knockout omitted — firmware mirrors per-pixel writes into
        // config.base.knockoutPixels in the CHAR_BASE_KNOCKOUT drain
        // and the blob drain uses config.asJsonDocument() as the base
        // for persistence, so the up-to-date knockout map is already
        // in the persisted JSON. Sending it here would only inflate
        // the encrypted payload past the 512-byte ATT cap when many
        // pixels are customized.
      };
    }
    if (_isShadeDirty(cur.shade, _original.shade)) {
      blob['shade'] = {
        'px': cur.shade.px,
        'bpp': cur.shade.bpp,
        'byteOrder': cur.shade.byteOrder,
        'colors': cur.shade.colors.map((c) => c.toHex()).toList(),
      };
    }
    if (_isHomeDirty(cur.home, _original.home)) {
      blob['homeMode'] = <String, dynamic>{
        'ssid': cur.home.ssid,
        'brightness': cur.home.brightness,
        'enabled': cur.home.enabled,
        // Presence-only home mode — no password field.
      };
    }
    // expressions[] omitted unconditionally — same rationale as base.knockout
    // above. CHAR_EXPRESSION_OP writes mirror into config.expressions
    // firmware-side, so the persisted state is already current. This is
    // what keeps the blob under the 512-byte ATT cap with 5+ expressions.
    if (blob.isEmpty) {
      // Nothing dirty — shouldn't have reached here (UI gates on isDirty)
      // but be defensive: no-op write would just trigger a reboot for no
      // reason.
      return;
    }

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
        _deviceId,
        BleUuids.controlService,
        BleUuids.settingsBlob,
        payload,
        allowLongWrite: true,
      );
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
    //
    //    Flip lampSaveStatus → true so ConnectingView shows
    //    "Saving changes…" during the reconnect window instead of the
    //    generic "Connecting…" message.
    ref.read(lampSaveStatusProvider(_deviceId).notifier).start();
    state = const AsyncLoading<ControlState>();
    Future<void>.delayed(const Duration(seconds: 5), () async {
      if (!ref.mounted) {
        // Best-effort cleanup if the notifier got disposed mid-reconnect.
        return;
      }
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
          shade: fresh.shade.colors.isEmpty
          ? _blackShade
          : fresh.shade.colors.first,
          base: fresh.base.colors[fresh.base.ac],
        );
        ref.read(lampSaveStatusProvider(_deviceId).notifier).stop();
      } catch (e, st) {
        if (ref.mounted) state = AsyncError(e, st);
        ref.read(lampSaveStatusProvider(_deviceId).notifier).stop();
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
        socialMode: cur.lamp.socialMode,
      ),
    ));
    _brightnessWriter?.schedule(Uint8List.fromList([v]));
  }

  Future<void> setShadeColor(LampColor color) async {
    final cur = state.value;
    if (cur == null) return;
    final colors = [color];
    state = AsyncData(cur.copyWith(
      shade: ShadeSection(
        px: cur.shade.px,
        bpp: cur.shade.bpp,
        byteOrder: cur.shade.byteOrder,
        colors: colors,
      ),
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
        byteOrder: cur.base.byteOrder,
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
        byteOrder: cur.base.byteOrder,
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
        byteOrder: cur.base.byteOrder,
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
      // Cached BleClient avoids a `ref.read` from a Timer callback
      // that could fire after the notifier's onDispose ran.
      unawaited(_safeWriteKnockout(_ble, index, v));
    });
  }

  Future<void> _safeWriteKnockout(
      BleClient ble, int index, int brightness) async {
    try {
      // Same write-without-response semantics as the color/brightness
      // coalescers — knockout writes are also slider-rate live preview.
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.baseKnockout,
        Uint8List.fromList([index, brightness]),
        withoutResponse: true,
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
        byteOrder: cur.base.byteOrder,
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
        socialMode: cur.lamp.socialMode,
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
        socialMode: cur.lamp.socialMode,
      ),
    ));
  }

  /// Personality. State-only; rides the global Save → settings_blob path
  /// (mirrors setLampName / setLampAdvancedEnabled). The lamp picks up
  /// the new mode after the post-save reboot.
  Future<void> setLampSocialMode(SocialMode mode) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      lamp: LampSection(
        name: cur.lamp.name,
        brightness: cur.lamp.brightness,
        advancedEnabled: cur.lamp.advancedEnabled,
        socialMode: mode,
      ),
    ));
  }

  /// Wipe the lamp back to factory defaults. Writes the
  /// `{factoryReset: true}` sentinel to settings_blob (encrypted with the
  /// CURRENT password — the lamp authenticates the request before
  /// clearing). Firmware clears its NVS namespace and reboots into the
  /// awaiting-adoption state.
  ///
  /// After the expected reboot-disconnect, removes this lamp from the
  /// inventory: it no longer has a password we know, and from the user's
  /// POV it's a fresh lamp again. The caller (UI) typically navigates
  /// back to the lamp picker.
  Future<void> factoryReset() async {
    final cur = state.value;
    if (cur == null) return;
    if (!cur.connected) return;

    final ble = ref.read(bleClientProvider);
    final inv = await ref.read(inventoryNotifierProvider.future);
    final entry = inv.firstWhere(
      (l) => l.id == _deviceId,
      orElse: () =>
          throw StateError('lamp $_deviceId not in inventory'),
    );
    final pw = entry.controlPassword ?? '';

    final blob = <String, dynamic>{'factoryReset': true};
    final blobJson = jsonEncode(blob);
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
        _deviceId,
        BleUuids.controlService,
        BleUuids.settingsBlob,
        payload,
        allowLongWrite: true,
      );
    } catch (e) {
      // Expected: the reboot disconnects mid-write.
      final msg = e.toString().toLowerCase();
      final looksLikeReboot =
          msg.contains('not connected') || msg.contains('disconnect');
      if (!looksLikeReboot) rethrow;
    }

    // Lamp is now factory-fresh — tear down everything tied to this
    // session so the reconnect machinery doesn't churn forever against
    // a lamp the user has deliberately abandoned.
    //
    // Sequence matters:
    //   1. cancel timers + the BLE-connected stream subscription so
    //      _onConnectionChange can't fire and re-schedule a reconnect;
    //   2. disconnect the link explicitly (the firmware is rebooting so
    //      this is a no-op on the wire, but it releases fbp's handle);
    //   3. remove from inventory so any UI that watches inventory drops
    //      this lamp;
    //   4. flip state to AsyncError with a sentinel so anything still
    //      watching this provider sees a terminal state rather than a
    //      stuck AsyncLoading.
    _reconnectTimer?.cancel();
    _connSub?.cancel();
    _connSub = null;
    try {
      await ble.disconnect(_deviceId);
    } catch (_) {
      // already-disconnected / lamp rebooting — both fine.
    }
    await ref
        .read(inventoryNotifierProvider.notifier)
        .remove(_deviceId);
    state = AsyncError(
      const FactoryResetSentinel(),
      StackTrace.current,
    );
  }

  /// Called by the connect-time password prompt when the user submits a
  /// password after build() threw [LampAuthRequiredException].
  ///
  /// Writes CHAR_AUTH with the user-entered password and re-probes the auth
  /// gate by reading lampSection. On success, persists the credential to
  /// inventory (so future reconnects skip the prompt) and invalidates the
  /// provider so build() reruns cleanly with the new password in inventory.
  /// On failure, throws [LampAuthRequiredException] without mutating
  /// inventory — the dialog surfaces it inline and lets the user retry.
  ///
  /// Reuses `_ble` / `_deviceId`, which build() set before it threw. The
  /// BLE link is still alive: ControlNotifier is keepAlive, so the
  /// disconnect onDispose hasn't fired.
  Future<void> submitConnectPassword(String pw) async {
    await AuthClient(ble: _ble)
        .authenticate(deviceId: _deviceId, password: pw);
    final bytes = await _ble.read(
        _deviceId, BleUuids.controlService, BleUuids.lampSection);
    if (bytes.isEmpty) {
      throw const LampAuthRequiredException();
    }
    await ref
        .read(inventoryNotifierProvider.notifier)
        .updatePassword(_deviceId, pw);
    ref.invalidateSelf();
  }

  /// Change the lamp's auth password. Writes a partial settings_blob with
  /// just `{lamp: {password: newPassword}}` — same path the onboarding
  /// claim uses to set the initial password. The lamp drains the write,
  /// commits the new password to NVS, and reboots; we then reconnect and
  /// reauth with the new password.
  ///
  /// Inventory is updated to the new password BEFORE the BLE write so the
  /// post-reboot reconnect picks up the new credentials. If the write
  /// fails (anything other than the expected reboot-disconnect), inventory
  /// rolls back to the old password.
  ///
  /// Mirrors `save()`'s reboot/reconnect cadence (5s delay, then reconnect
  /// + reauth + reread sections) and drives `lampSaveStatusProvider` so
  /// the UI shows "Saving changes…" instead of generic "Connecting…".
  Future<void> setLampPassword(String newPassword) async {
    final cur = state.value;
    if (cur == null) return;
    if (!cur.connected) return;

    final ble = ref.read(bleClientProvider);

    // Snapshot for rollback on real failure.
    final inv = await ref.read(inventoryNotifierProvider.future);
    final entry = inv.firstWhere(
      (l) => l.id == _deviceId,
      orElse: () =>
          throw StateError('lamp $_deviceId not in inventory'),
    );
    final oldPassword = entry.controlPassword;

    // Update inventory FIRST so the post-reboot reconnect uses the new
    // credentials. If the write fails (non-reboot error), roll back.
    await ref
        .read(inventoryNotifierProvider.notifier)
        .updatePassword(_deviceId, newPassword);

    final blob = <String, dynamic>{'lamp': {'password': newPassword}};
    final blobJson = jsonEncode(blob);
    final pw = oldPassword ?? '';
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
        _deviceId,
        BleUuids.controlService,
        BleUuids.settingsBlob,
        payload,
        allowLongWrite: true,
      );
    } catch (e) {
      final msg = e.toString().toLowerCase();
      final looksLikeReboot =
          msg.contains('not connected') || msg.contains('disconnect');
      if (!looksLikeReboot) {
        // Genuine failure — restore the old credentials so the next
        // reconnect attempt uses what the firmware still actually has.
        await ref
            .read(inventoryNotifierProvider.notifier)
            .updatePassword(_deviceId, oldPassword);
        rethrow;
      }
    }

    // Reuse the save()-style reconnect cadence: flip the saving banner,
    // drop state into AsyncLoading, wait for the lamp to come back, reauth
    // with the new password (already in inventory).
    ref.read(lampSaveStatusProvider(_deviceId).notifier).start();
    state = const AsyncLoading<ControlState>();
    Future<void>.delayed(const Duration(seconds: 5), () async {
      if (!ref.mounted) return;
      try {
        await ble.connect(_deviceId);
        await AuthClient(ble: ble).authenticate(
            deviceId: _deviceId, password: newPassword);
        final fresh = await _readSections(ble);
        if (!ref.mounted) return;
        _original = fresh;
        state = AsyncData(fresh);
        ref.read(lampSaveStatusProvider(_deviceId).notifier).stop();
      } catch (e, st) {
        if (ref.mounted) state = AsyncError(e, st);
        ref.read(lampSaveStatusProvider(_deviceId).notifier).stop();
      }
    });
  }

  Future<void> setHomeSsid(String ssid) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      home: HomeSection(
        ssid: ssid,
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
        brightness: clamped,
        enabled: cur.home.enabled,
      ),
    ));
    // Live-write via CHAR_BRIGHTNESS — the firmware routes the value to
    // homeMode.brightness vs lamp.brightness based on whether the app
    // has signalled it's on the Home Mode page (CHAR_HOME_MODE_FOCUS).
    // Calling setHomeBrightness while NOT on the Home Mode page would
    // (incorrectly) update lamp.brightness firmware-side; the UI only
    // wires this mutator into the Home Mode slider, so that path is
    // structurally avoided.
    _brightnessWriter?.schedule(Uint8List.fromList([clamped]));
  }

  Future<void> setHomeEnabled(bool enabled) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      home: HomeSection(
        ssid: cur.home.ssid,
        brightness: cur.home.brightness,
        enabled: enabled,
      ),
    ));
  }

  /// Pick the wire byte order for the base strip. Recognized values:
  /// `GRBW` (4 bpp), `GRB` (3 bpp), `BGR` (3 bpp). `bpp` is updated to
  /// match so the wire payload stays internally consistent for any
  /// future firmware that only reads `bpp`.
  Future<void> setBaseByteOrder(String order) async {
    final cur = state.value;
    if (cur == null) return;
    final normalized = _normalizeByteOrder(order);
    state = AsyncData(cur.copyWith(
      base: BaseSection(
        px: cur.base.px,
        ac: cur.base.ac,
        bpp: _bppForByteOrder(normalized),
        byteOrder: normalized,
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
        byteOrder: cur.base.byteOrder,
        colors: cur.base.colors,
        knockout: cur.base.knockout,
      ),
    ));
  }

  Future<void> setShadeByteOrder(String order) async {
    final cur = state.value;
    if (cur == null) return;
    final normalized = _normalizeByteOrder(order);
    state = AsyncData(cur.copyWith(
      shade: ShadeSection(
        px: cur.shade.px,
        bpp: _bppForByteOrder(normalized),
        byteOrder: normalized,
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
        byteOrder: cur.shade.byteOrder,
        colors: cur.shade.colors,
      ),
    ));
  }

  static String _normalizeByteOrder(String order) {
    final up = order.toUpperCase();
    // Keep the strict allow-list small. Adafruit_NeoPixel supports more
    // permutations; expand here once a strip actually needs one.
    if (up == 'GRBW' || up == 'GRB' || up == 'BGR') return up;
    return 'GRBW';
  }

  static int _bppForByteOrder(String order) =>
      order == 'GRBW' || order == 'BGRW' || order == 'RGBW' ? 4 : 3;

  // ---------------------------------------------------------------------------
  // Expressions
  // ---------------------------------------------------------------------------

  /// Add or update an expression. Live-previews via CHAR_EXPRESSION_OP — the
  /// firmware updates its in-memory expressionManager (not NVS); persistence
  /// to NVS happens on the next global Save via the settings_blob payload
  /// which carries the full expressions array.
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

  /// Remove the expression keyed by (type, target). Live-previews via
  /// CHAR_EXPRESSION_OP; NVS persistence happens on the next global Save.
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

  /// Live-previews `entry` in the firmware's in-memory expressionManager
  /// WITHOUT touching app state. Paired with [revertExpressionPreview] for
  /// the editor's Test action: write draft → trigger → revert on close.
  /// Because CHAR_EXPRESSION_OP doesn't persist to NVS, the revert is fully
  /// in-memory and leaves the saved state untouched.
  Future<void> previewExpression(ExpressionConfig entry) async {
    await _writeExpressionOp({'op': 'upsert', 'entry': entry.toJson()});
  }

  /// Restore the firmware's in-memory entry at (type, target) back to the
  /// last-loaded snapshot (`_original`). If the key didn't exist in the
  /// snapshot we issue a `remove` instead. Used by the editor to undo a
  /// transient Test-time preview.
  Future<void> revertExpressionPreview({
    required String type,
    required int target,
  }) async {
    ExpressionConfig? prev;
    for (final e in _original.expressions.expressions) {
      if (e.type == type && e.target == target) {
        prev = e;
        break;
      }
    }
    if (prev != null) {
      await _writeExpressionOp({'op': 'upsert', 'entry': prev.toJson()});
    } else {
      await _writeExpressionOp({
        'op': 'remove',
        'type': type,
        'target': target,
      });
    }
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
      // Advanced mode is session-only — drop the unlock the moment the BLE
      // session ends. User must re-do the tap gesture after reconnect.
      ref.read(advancedSessionProvider(_deviceId).notifier).disable();
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
      // Same canary as build(): if the firmware still returns empty bytes
      // after our auth attempt, the stored password no longer works (e.g.
      // it was changed on another device). Drop to error so the UI re-
      // prompts instead of leaving the user in a silent-write-rejected
      // state with the banner clearing as if everything was fine.
      final canaryBytes = await ble.read(
          _deviceId, BleUuids.controlService, BleUuids.lampSection);
      if (canaryBytes.isEmpty) {
        state = AsyncError(
            const LampAuthRequiredException(), StackTrace.current);
        return;
      }
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
