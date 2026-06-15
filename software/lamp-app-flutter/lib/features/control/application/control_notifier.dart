import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/widgets.dart';
import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client.dart';
import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/lamp_crypto.dart';
import '../../../core/ble/uuids.dart';
import '../../../core/ble/write_coalescer.dart';
import '../../../core/lifecycle/app_lifecycle.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../domain/lamp_color.dart';
import '../domain/sections.dart';
import '../../social/domain/social_mode.dart';
import 'advanced_session.dart';
import 'auth_client.dart';
import 'commit_section.dart';
import 'control_state.dart';
import 'lamp_auth_required_exception.dart';
import 'lamp_save_status.dart';

part 'control_notifier.g.dart';

/// Per-surface enum for [ControlNotifier.setEditSession]. Bit values
/// match the firmware-side CHAR_EDIT_SESSION mask:
///   - base = 0x01
///   - shade = 0x02
///   - brightness = 0x04
enum EditSurface {
  base(0x01),
  shade(0x02),
  brightness(0x04);

  const EditSurface(this.bit);
  final int bit;
}

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
  /// Single-slot guard against concurrent reconnect attempts. Set true at
  /// the top of `_tryReconnect`, cleared in `finally`. Lets us safely
  /// kick `_tryReconnect` from BOTH the scheduled timer (`_scheduleReconnect`)
  /// AND the watchConnected→true edge (`_onConnectionChange`) without
  /// stacking ble.connect + auth + canary calls on top of each other.
  bool _reconnectInFlight = false;
  static const _reconnectDelays = [500, 1000, 2000, 4000, 8000]; // ms, capped
  /// Max number of consecutive reconnect attempts before we give up and
  /// surface an error state instead of polling forever (audit perf-M4).
  /// 30 attempts × the capped 8 s delay = ~4 minutes of background
  /// reconnect work. Past that, the lamp is almost certainly out of
  /// range and continuing to retry just keeps the radio warm + drains
  /// the user's battery. User can pull to refresh / re-tap the lamp to
  /// reset the loop.
  static const _maxReconnectAttempts = 30;

  // Inventory "last-seen color" debouncer. We don't need to persist every
  // slider tick — only the trailing value. The previous code was awaiting a
  // SharedPreferences write + an inventory-state notification per tick,
  // which dominated frame time during a drag. We collapse all the writes
  // inside a window into one disk write at the trailing edge.
  Timer? _seenFlushTimer;
  LampColor? _pendingSeenShade;
  LampColor? _pendingSeenBase;
  static const _seenFlushDelay = Duration(milliseconds: 500);

  // Commit debouncer. Scheduled after user fences (slider release, picker
  // accept). Trailing-edge: each new call cancels the previous timer so
  // rapid calls collapse to one BLE write. Flushed synchronously on
  // notifier dispose and on AppLifecycleState.paused.
  /// Debounce window after the last user fence (slider release, picker
  /// accept) before commit fires. 500ms matches the spec — feels instant
  /// after release, generous enough that incremental taps collapse to
  /// one commit.
  static const Duration _commitDebounce = Duration(milliseconds: 500);
  Timer? _commitDebounceTimer;
  CommitSection? _pendingCommitSection;
  // Cached from the probe result in build(). Lets _flushPendingCommit
  // check Phase A capability without touching `ref` (which is forbidden
  // inside ref.onDispose callbacks).
  bool _hasCommitChar = false;

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

  /// Wait for the lamp to reboot and reconnect after a settingsBlob /
  /// password write, then reauthenticate, re-read every section, and
  /// update [_original] + [state]. Calls [postReload] after the new
  /// state has landed.
  ///
  /// Replaces the prior fixed `Future.delayed(5s)` pattern. That was a
  /// timing race: the lamp's boot+BLE-up cycle takes ~3-10s and varies
  /// per save, so a fixed 5s timer often fires before the BLE link is
  /// re-established and `discoverServices` throws
  /// "Device is disconnected". The previous catch block surfaced
  /// `AsyncError` and left `_original` stale — the dirty-pill-stuck bug
  /// observed 2026-06-04.
  ///
  /// We now poll with 1s spacing, up to [maxAttempts] (~12s total wait).
  /// Each attempt: `ble.connect` → `authenticate(password)` →
  /// `_readSections`. Transient disconnect-like errors retry. Non-
  /// transient errors and final-attempt failures surface as `AsyncError`.
  Future<void> _awaitReconnectAndReload({
    required BleClient ble,
    required String password,
    required Future<void> Function(ControlState fresh) postReload,
    int maxAttempts = 12,
  }) async {
    // Initial grace: give the lamp a head start on the reboot so the
    // first attempt isn't guaranteed to race the disconnect. ~2s lets
    // the firmware fade-out complete and the actual reboot kick in.
    await Future<void>.delayed(const Duration(seconds: 2));

    ControlState? fresh;
    Object? lastError;
    StackTrace? lastStack;
    for (var attempt = 0; attempt < maxAttempts; attempt++) {
      if (!ref.mounted) return;
      try {
        await ble.connect(_deviceId);
        await AuthClient(ble: ble).authenticate(
            deviceId: _deviceId, password: password);
        fresh = await _readSections(ble);
        break;
      } catch (e, st) {
        lastError = e;
        lastStack = st;
        // Disconnect / not-connected go through the typed path now
        // (BleDisconnectedException from FbpBleClient). The remaining
        // transient signals — discoverServices flakes and connect/auth
        // timeouts — are still detected by message inspection because
        // fbp doesn't surface them as discrete types.
        final msg = e.toString().toLowerCase();
        final isTransient = isBleDisconnectError(e) ||
            msg.contains('discoverservices') ||
            msg.contains('timeout');
        if (!isTransient) break;
        await Future<void>.delayed(const Duration(seconds: 1));
      }
    }

    if (!ref.mounted) return;
    if (fresh == null) {
      state = AsyncError(
          lastError ?? Exception('lamp did not reconnect after save'),
          lastStack ?? StackTrace.current);
      ref.read(lampSaveStatusProvider(_deviceId).notifier).stop();
      return;
    }

    _original = fresh;
    state = AsyncData(fresh);
    try {
      await postReload(fresh);
    } catch (e, st) {
      // Don't roll back _original on postReload failure — the section
      // data is valid; postReload is best-effort housekeeping.
      if (ref.mounted) state = AsyncError(e, st);
    }
    ref.read(lampSaveStatusProvider(_deviceId).notifier).stop();
  }

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

    // Skip the cold GATT connect when a pre-warm has already established
    // the link (BleClient.prewarm wired into the BLE adv stream — see
    // nearby_lamps_notifier.dart). Saves the 200-500 ms `device.connect`
    // handshake + 100-400 ms `discoverServices` round-trip on the
    // common "user opens app, taps a nearby paired lamp" path. Cold-tap
    // path (lamp out of range until just before the tap) still pays
    // those costs normally.
    if (!ble.isConnected(deviceId)) {
      await ble.connect(deviceId);
    }
    // Register disconnect now — only after a successful connect. If connect
    // itself threw, there's nothing to tear down, and registering this
    // earlier would crash dispose on platforms where the BLE client can't
    // run (e.g. flutter_blue_plus in unit tests). If auth or section reads
    // throw below, this still fires on container teardown.
    //
    // The commit flush is bundled here (before the ble.disconnect call)
    // so that _flushPendingCommit fires while the link is still open.
    // ref.onDispose callbacks are called FIFO; if flush were registered
    // later it would race with the disconnect.
    ref.onDispose(() {
      _flushPendingCommit();
      ble.disconnect(deviceId);
    });
    await AuthClient(ble: ble)
        .authenticate(deviceId: deviceId, password: lamp.controlPassword);

    // Auth-gate canary. Post-commit 71415e0 the firmware returns empty bytes
    // from lampSection on unauthenticated reads (so an unauth'd peer can't
    // exfiltrate the password embedded in the section blob). If our stored
    // credential is missing or stale, surface that as a typed sentinel the
    // UI can catch and convert into a password prompt — otherwise the empty
    // bytes propagate into jsonDecode and surface as a generic FormatException.
    final canaryBytes = await ble.readSection(deviceId, 'lamp');
    if (canaryBytes.isEmpty) {
      throw const LampAuthRequiredException();
    }

    // Probe for Phase A firmware capability. GATT services are already
    // discovered by this point (authenticate() drives the first I/O),
    // so this re-uses the _serviceCache and costs no extra radio round-
    // trip. Returns false on any error — safe fallback to legacy flow.
    // Does NOT persist to SharedPreferences: hasCommitChar is a
    // session-local capability flag that must be re-probed on every
    // connect (firmware can be updated between sessions). Null entries
    // in existing inventory parse cleanly; read sites use `?? false`.
    final hasCommit = await ble.probeHasCommitChar(deviceId);
    _hasCommitChar = hasCommit; // cached for dispose-safe _flushPendingCommit
    await _inv.updateHasCommitChar(deviceId, hasCommitChar: hasCommit);

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
      } catch (e) {
        // A live-preview write throwing a disconnect-shaped error is the
        // canonical "link has zombified" signal — fbp's connectionState
        // stream sometimes misses the false edge (backgrounded socket
        // teardown, gatts_if slot leak). Treat it as the disconnect we
        // never observed and kick the reconnect ladder so the user
        // doesn't have to force-stop the app to recover.
        if (isBleDisconnectError(e) && ref.mounted) {
          // Notifier dispose during a live-preview write happens routinely
          // (lamp switch, back-nav, post-save reboot) — `safeWrite`'s
          // own onWrite closure outlives ref. Guard against mutating
          // state on a disposed notifier.
          _onConnectionChange(false);
        }
        // Other exception types (e.g. encryption-required surfaced mid-
        // session, or a transient write rejection) are still dropped —
        // a live-preview write tearing the UI down on an isolated failure
        // would be a worse outcome than the missed paint.
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
      // Cancel the debounce timer — the commit flush itself is handled in
      // the earlier onDispose (bundled with disconnect) so it fires while
      // the BLE link is still open. Cancelling here just stops a dangling
      // timer from firing after disconnect.
      _commitDebounceTimer?.cancel();
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

    // Probe the BLE link whenever the app comes back to the foreground.
    // Backgrounded apps can have their GATT connection torn down by the
    // OS (Android process priority, iOS suspend); fbp's connectionState
    // stream doesn't always emit the `false` edge in those cases, so a
    // bare _onConnectionChange listener stays stuck on `true` and any
    // user interaction silently fails. The probe forces a real GATT
    // round-trip — if the link is dead, the read throws
    // BleDisconnectedException and we kick the reconnect ladder.
    ref.listen<AppLifecycleState>(appLifecycleStateProvider, (prev, next) {
      if (next == AppLifecycleState.resumed && prev != next) {
        _probeLink();
      }
      if (next == AppLifecycleState.paused) {
        _flushPendingCommit();
      }
    });

    await _updateSeen(
      shade: fresh.shade.colors.isEmpty
          ? LampColor.black
          : fresh.shade.colors.first,
      base: fresh.base.colors[fresh.base.ac],
    );

    // Mirror the lamp's current display name into the inventory cache so
    // the lamp picker stays in sync with renames that happened on another
    // phone, or that survived a factory-reset + re-adopt. The control
    // screen's "Hello my name is:" header reads live state and is always
    // correct; the picker reads InventoryLamp.name and would otherwise
    // stay frozen at adopt-time.
    await _inv.updateName(deviceId, fresh.lamp.name);

    // Self-heal: if a previous session left the firmware's configurator
    // behaviors stuck in disabled=true (the test_expression / complete
    // protocol can leak that state — see ExpressionEditorScreen.dispose),
    // re-enable them on every connect. Cheap one-shot write; safe no-op
    // when the configurators are already enabled.
    await _completeExpressionTest(ble);

    _original = fresh;
    return fresh;
  }

  /// Reads every section via the page protocol and returns a fresh
  /// ControlState. Assumes the BLE link is connected and authenticated.
  /// Used by both build() (initial load) and save() (post-reboot reload).
  Future<ControlState> _readSections(BleClient ble) async {
    Future<Map<String, dynamic>> readJsonSection(String name) async {
      final bytes = await ble.readSection(_deviceId, name);
      return jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
    }

    Future<List<dynamic>> readJsonListSection(String name) async {
      final bytes = await ble.readSection(_deviceId, name);
      return jsonDecode(utf8.decode(bytes)) as List<dynamic>;
    }

    final lampJson = await readJsonSection('lamp');
    final baseJson = await readJsonSection('base');
    final shadeJson = await readJsonSection('shade');
    final homeJson = await readJsonSection('home');
    final exprList = await readJsonListSection('expr');
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
    } on BleDisconnectedException {
      // Expected: the reboot drops the link mid-write. Treated as success.
    } catch (e) {
      // Non-link failures — rethrow so the UI can surface them.
      if (!isBleDisconnectError(e)) rethrow;
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

    final invForSave = await ref.read(inventoryNotifierProvider.future);
    final lampForSave = invForSave.firstWhere(
      (l) => l.id == _deviceId,
      orElse: () => throw StateError('lamp $_deviceId not in inventory'),
    );

    // Don't await — the helper drives the AsyncLoading → AsyncData /
    // AsyncError transition + stops the save-status spinner internally,
    // matching the prior unawaited Future.delayed structure so the
    // caller returns promptly without blocking the UI.
    unawaited(_awaitReconnectAndReload(
      ble: ble,
      password: lampForSave.controlPassword ?? '',
      postReload: (fresh) async {
        await _updateSeen(
          shade: fresh.shade.colors.isEmpty
              ? LampColor.black
              : fresh.shade.colors.first,
          base: fresh.base.colors[fresh.base.ac],
        );
        // Sync the inventory's cached display name with whatever the firmware
        // actually persisted — handles renames via this app and is defensive
        // against any rename-rejection the firmware might add later.
        await _inv.updateName(_deviceId, fresh.lamp.name);
      },
    ));
  }

  /// Phase B.9.1 commit fence. On lamps running Phase A firmware
  /// (hasCommitChar==true) writes a single byte to CHAR_COMMIT — the
  /// firmware persists current RAM state to NVS. On pre-Phase-A lamps
  /// (hasCommitChar==false) this is a no-op; the Save pill remains the
  /// persistence trigger.
  ///
  /// The [section] argument is local knowledge for the caller (so each
  /// per-pane site declares what it just edited). It is NOT serialized
  /// to the wire — CHAR_COMMIT is parameterless. Reserved for the
  /// (rare) per-pane fallback that calls `writeSettingsBlob` directly
  /// when the caller explicitly wants partial-blob semantics on a
  /// pre-Phase-A lamp.
  ///
  /// DESIGN DEVIATION FROM SPEC B.3: the spec describes a fallback that
  /// synthesizes a partial settings_blob from state.value when
  /// hasCommitChar==false. This implementation takes a simpler
  /// interpretation — pre-Phase-A lamps fall through to the legacy
  /// Save-pill flow (the pill is still visible on those lamps per
  /// Task 12). No fallback synthesis. The synthesized blob would
  /// trigger a reboot on pre-Phase-A firmware (which ignores
  /// reboot:false), which is exactly the UX the user said they don't
  /// want for slider releases / picker accepts.
  Future<void> commit(CommitSection section) async {
    final inv = await ref.read(inventoryNotifierProvider.future);
    final entry = inv.firstWhere(
      (l) => l.id == _deviceId,
      orElse: () => throw StateError('lamp $_deviceId not in inventory'),
    );
    // `?? false` defaults null (not-yet-probed) to legacy behavior.
    if (!(entry.hasCommitChar ?? false)) return;

    final ble = ref.read(bleClientProvider);
    try {
      await ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.commit,
        Uint8List.fromList([0x01]),
      );
    } catch (e, st) {
      debugPrint('controlNotifier.commit(${section.name}) failed: $e\n$st');
      rethrow;
    }
  }

  /// Optimistic-update wrapper. Captures the current state, applies
  /// the [transform] immediately so the UI feels instant, then awaits
  /// the [commit] callback that does the BLE write(s). On exception,
  /// restores the captured state and rethrows.
  ///
  /// Per-pane mutators wrap their work in this helper to inherit
  /// snackbar-friendly failure semantics without each call site
  /// duplicating the try/catch dance.
  Future<void> _mutate(
    ControlState Function(ControlState) transform,
    Future<void> Function() commit,
  ) async {
    final prev = state.value;
    if (prev == null) return;
    state = AsyncData(transform(prev));
    try {
      await commit();
    } catch (e) {
      state = AsyncData(prev);
      rethrow;
    }
  }

  /// Test-only access to [_mutate] so unit tests can exercise the
  /// revert path without going through a public mutator.
  @visibleForTesting
  Future<void> mutateForTest(
    ControlState Function(ControlState) transform,
    Future<void> Function() commit,
  ) =>
      _mutate(transform, commit);

  /// Schedule a debounced commit. If called again before the window
  /// expires, the timer is cancelled and rescheduled (trailing-edge
  /// behavior). The [section] is purely local-knowledge metadata; see
  /// [commit].
  void _scheduleCommitDebounced(CommitSection section) {
    _commitDebounceTimer?.cancel();
    _pendingCommitSection = section;
    _commitDebounceTimer = Timer(_commitDebounce, () async {
      _commitDebounceTimer = null;
      final s = _pendingCommitSection;
      _pendingCommitSection = null;
      if (s == null) return;
      try {
        await commit(s);
      } catch (e, st) {
        debugPrint('controlNotifier._scheduleCommitDebounced failed: $e\n$st');
        // No UI surface here — the user has already moved on; the
        // next commit attempt will retry. (The Save pill / isDirty
        // machinery still covers the persistence gap during
        // transition.)
      }
    });
  }

  /// Synchronously force-flush a pending debounced commit. Called from
  /// dispose + AppLifecycleState.paused so a quick edit-then-leave
  /// doesn't lose the user's last change.
  ///
  /// IMPORTANT: This method MUST NOT call `ref.read(...)` or access any
  /// Riverpod notifier `.state` — it runs from `ref.onDispose` where
  /// Riverpod forbids ref access. Uses cached `_ble` and `_hasCommitChar`
  /// fields instead (same pattern as `_flushSeen` → `_updateSeen` → `_inv`).
  void _flushPendingCommit() {
    final s = _pendingCommitSection;
    if (s == null) return;
    _commitDebounceTimer?.cancel();
    _commitDebounceTimer = null;
    _pendingCommitSection = null;

    // Guard: pre-Phase-A lamps don't have CHAR_COMMIT; no-op mirrors
    // commit()'s own hasCommitChar check.
    if (!_hasCommitChar) return;

    // Fire-and-forget — caller can't await us, but the BLE write
    // itself is async. If the link is gone, the commit fails and the
    // user sees the snackbar on next page load.
    unawaited(_ble.write(
      _deviceId,
      BleUuids.controlService,
      BleUuids.commit,
      Uint8List.fromList([0x01]),
    ).catchError((e, st) {
      debugPrint('controlNotifier._flushPendingCommit failed: $e\n$st');
    }));
  }

  /// Public alias for tests that want to drive the debounce machinery
  /// without going through a per-pane mutator (which won't exist until
  /// Task 6+).
  @visibleForTesting
  void scheduleCommitDebouncedForTest(CommitSection s) =>
      _scheduleCommitDebounced(s);

  /// Writes an arbitrary settings_blob JSON map to the lamp.
  ///
  /// The `reboot` flag is merged into the map before encryption.
  /// Phase A firmware reads the flag and skips its reboot cycle when
  /// `reboot == false`; pre-Phase-A firmware always reboots and simply
  /// ignores the unknown key.
  ///
  /// Use `reboot: false` for discrete Phase B mutators (rename,
  /// personality, home toggle) where only CHAR_COMMIT follows.
  /// Use `reboot: true` (the default) for Advanced LED changes and
  /// factory-reset-adjacent writes that must trigger a full reboot.
  ///
  /// When `reboot == true` the expected BleDisconnectedException
  /// (firmware drops the link during its reboot cycle) is swallowed.
  /// When `reboot == false` a disconnect is a real error and is
  /// rethrown so the caller can surface it.
  ///
  /// Throws on BLE write failure (caller wraps + snackbars).
  Future<void> writeSettingsBlob(
    Map<String, dynamic> blob, {
    bool reboot = true,
  }) async {
    final ble = ref.read(bleClientProvider);
    final inv = await ref.read(inventoryNotifierProvider.future);
    final lamp = inv.firstWhere(
      (l) => l.id == _deviceId,
      orElse: () => throw StateError('lamp $_deviceId not in inventory'),
    );
    final pw = lamp.controlPassword ?? '';

    final payloadBlob = <String, dynamic>{
      ...blob,
      'reboot': reboot,
    };
    final blobJson = jsonEncode(payloadBlob);

    final payload = pw.isEmpty
        ? Uint8List.fromList([
            LampCrypto.magicPlaintext,
            ...utf8.encode(blobJson),
          ])
        : await LampCrypto.encryptOp(
            op: payloadBlob,
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
    } on BleDisconnectedException {
      // Expected when reboot==true — firmware drops the link mid-write.
      // For reboot==false this is a real disconnect: rethrow so the
      // caller can surface it.
      if (!reboot) rethrow;
    } catch (e) {
      if (!isBleDisconnectError(e)) rethrow;
      // Generic disconnect wrapper (non-typed path): same semantics.
      if (!reboot) rethrow;
    }
  }

  /// Signal to the lamp that the operator is actively editing colours
  /// (or brightness) for [surface]. The lamp uses this to drop
  /// wisp-sourced overrides on that surface for the duration of the
  /// session, so a wisp paint frame mid-drag doesn't fight the user's
  /// pick. Always pair an `open=true` call with a matching `open=false`
  /// when the picker / drag closes (use `try/finally`). The lamp's
  /// own onDisconnect handler also clears every flag as a defensive
  /// sweep, so a forgotten close (app crash, force-stop) recovers on
  /// the next reconnect.
  ///
  /// Fire-and-forget: a write failure here is harmless (the wisp's own
  /// fade machinery still recovers when the next paint frame arrives),
  /// so errors are swallowed quietly. NOT routed through WriteCoalescer
  /// — open/close are one-shot per event, no throttling needed.
  Future<void> setEditSession(EditSurface surface, bool open) async {
    try {
      await _ble.write(
        _deviceId,
        BleUuids.controlService,
        BleUuids.editSession,
        Uint8List.fromList([surface.bit, open ? 1 : 0]),
        withoutResponse: true,
      );
    } catch (_) {
      // best-effort
    }
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
        fwVersion: cur.lamp.fwVersion,
        fwChannel: cur.lamp.fwChannel,
      ),
    ));
    _brightnessWriter?.schedule(Uint8List.fromList([v]));
  }

  /// Called from the brightness slider's `onChangeEnd` to schedule a
  /// debounced commit. Public wrapper around the private debounce
  /// so the widget layer doesn't need access to CommitSection.
  void scheduleBrightnessCommit() {
    _scheduleCommitDebounced(CommitSection.lamp);
  }

  /// Called from the knockout screen after each per-pixel edit (and on
  /// screen-back force-flush) to schedule a debounced commit. Same
  /// debounce window as brightness.
  void scheduleKnockoutCommit() {
    _scheduleCommitDebounced(CommitSection.baseKnockout);
  }

  /// Synchronous force-flush exposed for the knockout screen's
  /// PopScope hook. The debounce timer may not have fired yet and the
  /// notifier's onDispose may not fire at route-pop if the notifier
  /// is app-scoped.
  void flushKnockoutCommit() {
    _flushPendingCommit();
  }

  Future<void> setShadeColor(LampColor color) async {
    // Single-color convenience: wraps the color in a 1-element list and
    // routes through `setShadeColors`. Existing callers (expression
    // editor live-preview, lamp_preview thumbnails) keep their old
    // signature.
    return setShadeColors([color]);
  }

  Future<void> setShadeColors(List<LampColor> colors) async {
    final cur = state.value;
    if (cur == null) return;
    state = AsyncData(cur.copyWith(
      shade: ShadeSection(
        px: cur.shade.px,
        bpp: cur.shade.bpp,
        byteOrder: cur.shade.byteOrder,
        colors: colors,
      ),
    ));
    _shadeColorsWriter?.schedule(_encodeColors(colors));
    // Inventory "last seen" cache mirrors the first stop — same shape as
    // the pre-gradient single-color path, so the lamp picker's swatch
    // preview stays representative.
    if (colors.isNotEmpty) {
      _queueSeen(shade: colors.first);
    }
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
    // NEW: schedule a debounced commit so the change persists on
    // Phase A lamps. No-op on pre-Phase-A (commit() returns early).
    _scheduleCommitDebounced(CommitSection.baseKnockout);
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
    // Serialize the per-pixel writes (audit perf-M3). Pre-fix this
    // fan-out fired up to ~144 unawaited writes in a single tick,
    // queue-bursting the BLE TX queue against a ~33 Hz drain. Now we
    // walk them sequentially so the queue stays shallow + the radio
    // doesn't drop frames; total wall-clock is still fast (one tx per
    // ~30 ms ≈ 4.5 s worst case for the full 144) and the user only
    // hits this from "Reset all" which they don't repeat-spam.
    final ble = ref.read(bleClientProvider);
    unawaited(() async {
      for (final i in edited) {
        await _safeWriteKnockout(ble, i, 100);
      }
    }());
    // Schedule a debounced commit so the clear persists on Phase A lamps.
    _scheduleCommitDebounced(CommitSection.baseKnockout);
  }

  // ---------------------------------------------------------------------------
  // Setup-screen mutators — Phase A: immediate writeSettingsBlob commit;
  // pre-Phase-A: state-only, ride the global Save → settingsBlob path.
  // ---------------------------------------------------------------------------

  Future<void> setLampName(String name) async {
    await _mutate(
      (s) => s.copyWith(
        lamp: LampSection(
          name: name,
          brightness: s.lamp.brightness,
          advancedEnabled: s.lamp.advancedEnabled,
          socialMode: s.lamp.socialMode,
          fwVersion: s.lamp.fwVersion,
          fwChannel: s.lamp.fwChannel,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere(
          (l) => l.id == _deviceId,
          orElse: () => throw StateError('lamp $_deviceId not in inventory'),
        );
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob({'lamp': {'name': name}}, reboot: false);
        }
        // Inventory cache update fires for BOTH paths so the AppBar
        // LampChip title is correct without waiting for a reload.
        await ref
            .read(inventoryNotifierProvider.notifier)
            .updateName(_deviceId, name);
      },
    );
  }

  Future<void> setLampAdvancedEnabled(bool v) async {
    await _mutate(
      (s) => s.copyWith(
        lamp: LampSection(
          name: s.lamp.name,
          brightness: s.lamp.brightness,
          advancedEnabled: v,
          socialMode: s.lamp.socialMode,
          fwVersion: s.lamp.fwVersion,
          fwChannel: s.lamp.fwChannel,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere((l) => l.id == _deviceId);
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob(
              {'lamp': {'advancedEnabled': v}}, reboot: false);
        }
      },
    );
  }

  /// Personality pill. On Phase A lamps, commits immediately via
  /// writeSettingsBlob (reboot: false). On pre-Phase-A lamps, state-only
  /// — rides the global Save → settings_blob path. The lamp picks up the
  /// new mode after the post-save reboot.
  Future<void> setLampSocialMode(SocialMode mode) async {
    await _mutate(
      (s) => s.copyWith(
        lamp: LampSection(
          name: s.lamp.name,
          brightness: s.lamp.brightness,
          advancedEnabled: s.lamp.advancedEnabled,
          socialMode: mode,
          fwVersion: s.lamp.fwVersion,
          fwChannel: s.lamp.fwChannel,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere((l) => l.id == _deviceId);
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob(
              {'lamp': {'socialMode': mode.wire}}, reboot: false);
        }
      },
    );
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
    } on BleDisconnectedException {
      // Expected: the reboot drops the link mid-write.
    } catch (e) {
      if (!isBleDisconnectError(e)) rethrow;
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
    unawaited(_connSub?.cancel());
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
    final bytes = await _ble.readSection(_deviceId, 'lamp');
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
    // SECURITY (accepted threat T2): when no prior password exists
    // (factory state, post-reset), the new password is written in
    // plaintext. There's no shared secret yet to derive an AES key
    // from. A passive BLE sniffer in range at adoption captures this
    // lamp's new admin credential. The only real fix — fleet-wide
    // mesh authentication — was deliberately rejected. See
    // docs/superpowers/notes/2026-06-10-accepted-security-threats.md.
    // Threat is bounded by physical proximity at adoption time.
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
    } on BleDisconnectedException {
      // Expected: the reboot drops the link mid-write.
    } catch (e) {
      if (isBleDisconnectError(e)) return;
      // Genuine failure — restore the old credentials so the next
      // reconnect attempt uses what the firmware still actually has.
      await ref
          .read(inventoryNotifierProvider.notifier)
          .updatePassword(_deviceId, oldPassword);
      rethrow;
    }

    // Reuse the save()-style reconnect cadence: flip the saving banner,
    // drop state into AsyncLoading, then defer to _awaitReconnectAndReload
    // which polls connect+auth+read with backoff (replaces the brittle
    // fixed 5s delay — see the helper's docstring).
    ref.read(lampSaveStatusProvider(_deviceId).notifier).start();
    state = const AsyncLoading<ControlState>();
    unawaited(_awaitReconnectAndReload(
      ble: ble,
      password: newPassword,
      postReload: (fresh) async {
        // Same inventory-name sync as save(): user may have renamed in the
        // same edit batch that bundled the password change.
        await _inv.updateName(_deviceId, fresh.lamp.name);
      },
    ));
  }

  Future<void> setHomeSsid(String ssid) async {
    await _mutate(
      (s) => s.copyWith(
        home: HomeSection(
          ssid: ssid,
          brightness: s.home.brightness,
          enabled: s.home.enabled,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere((l) => l.id == _deviceId);
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob({'homeMode': {'ssid': ssid}}, reboot: false);
        }
      },
    );
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
    // Phase B: schedule debounced commit so the value persists without
    // the Save pill on Phase A lamps.
    _scheduleCommitDebounced(CommitSection.homeMode);
  }

  Future<void> setHomeEnabled(bool enabled) async {
    await _mutate(
      (s) => s.copyWith(
        home: HomeSection(
          ssid: s.home.ssid,
          brightness: s.home.brightness,
          enabled: enabled,
        ),
      ),
      () async {
        final inv = await ref.read(inventoryNotifierProvider.future);
        final entry = inv.firstWhere((l) => l.id == _deviceId);
        if (entry.hasCommitChar ?? false) {
          await writeSettingsBlob(
              {'homeMode': {'enabled': enabled}}, reboot: false);
        }
      },
    );
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
    // Firmware-side, applyExpressionOpLocal + persistConfig has now written
    // the new entry to NVS (added 2026-06-13 — see standard_lamp.cpp's
    // expressionOp drain). Mirror that on the app side by advancing
    // `_original.expressions` so isDirty clears immediately and the UI's
    // "unsaved changes" indicator goes away. The lamp_shell isDirty pill
    // was sticking on every Update click because `_original` only got
    // refreshed via the full settingsBlob save + reboot path, which the
    // expressions screen never invokes.
    _original = _original.copyWith(
      expressions: ExpressionsSection(expressions: next),
    );
  }

  /// Remove the expression keyed by (type, target). Live-previews via
  /// CHAR_EXPRESSION_OP; the firmware persists to NVS immediately
  /// (added 2026-06-13).
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
    // Mirror the firmware-side NVS persist on the app side — same reason
    // as upsertExpression above. Without this the lamp_shell isDirty
    // pill stays lit after a delete.
    _original = _original.copyWith(
      expressions: ExpressionsSection(expressions: next),
    );
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
      // fbp's connectionState emitted `true` mid-reconnect — but the
      // GATT services aren't necessarily discovered yet (observed on
      // hardware: a 4-second window where writes failed with
      // "primary service not found" before the canary lands).
      //
      // Don't flip the UI to "connected" on this edge — _tryReconnect's
      // canary read is the truthful "the link can actually be used"
      // signal. Kick _tryReconnect immediately rather than waiting for
      // the soft 500ms reconnect timer; the in-flight guard prevents
      // doubling up with a timer-scheduled run that's already running.
      _reconnectTimer?.cancel();
      unawaited(_tryReconnect());
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

  /// Fires a no-op GATT read against the lamp to verify the link is
  /// actually alive. Used by the foreground-resume listener: fbp may
  /// still report `isConnected == true` for a connection the OS killed
  /// while the app was backgrounded, but any real I/O immediately
  /// throws BleDisconnectedException. We surface that as the disconnect
  /// edge we never observed, and the existing _onConnectionChange path
  /// schedules a reconnect.
  ///
  /// No-op when:
  ///   - notifier state is still loading / errored (nothing connected yet)
  ///   - we're already in a reconnect cycle (banner showing attempts)
  Future<void> _probeLink() async {
    final cur = state.value;
    if (cur == null || !cur.connected) return;
    final ble = ref.read(bleClientProvider);
    if (!ble.isConnected(_deviceId)) {
      // fbp itself has dropped the link — surface the missing false edge.
      _onConnectionChange(false);
      return;
    }
    try {
      // Cheap real GATT op: read the lamp section (small payload, part
      // of the cold-start sweep, no side effects). If this throws a
      // disconnect-shaped error, the slot is zombified.
      await ble.readSection(_deviceId, 'lamp');
    } on BleDisconnectedException {
      // The await above is the dispose window: the user can switch
      // lamps mid-probe, disposing the notifier. Touching state after
      // that throws. The `mounted` check matches the pattern in
      // _tryReconnect's catch.
      if (!ref.mounted) return;
      _onConnectionChange(false);
    } catch (_) {
      // Any other error (transient read failure, encryption etc.) is
      // not a clean disconnect signal. Don't disturb the connection
      // state — the next user action will surface a real failure if
      // the link is actually dead.
    }
  }

  /// Reconnect attempt at which we escalate to `cycleAdapter`. The soft
  /// reconnect ladder is good for clean link drops (lamp reboot,
  /// transient RF loss); after this many failures we assume the
  /// Android `gatts_if` slot has zombified and force a soft-cycle
  /// (explicit disconnect + delay + reconnect) before the next attempt.
  /// Pre-this-attempt, plain reconnect; on this attempt, cycle then
  /// reconnect.
  static const int _cycleAdapterAttempt = 3;

  void _scheduleReconnect() {
    final cur = state.value;
    if (cur == null) return;
    final attempt = cur.reconnectAttempt;
    // Park after the cap (audit perf-M4). The reconnect loop ran
    // forever pre-fix — a wandered-off lamp would keep polling every
    // 8s indefinitely, draining battery for nothing. User can re-tap
    // the lamp in MyLamps to reset the attempt counter.
    if (attempt >= _maxReconnectAttempts) {
      state = AsyncError(
        const BleNotConnected('reconnect cap reached'),
        StackTrace.current,
      );
      return;
    }
    final delayMs = _reconnectDelays[
        attempt < _reconnectDelays.length ? attempt : _reconnectDelays.length - 1];
    _reconnectTimer?.cancel();
    _reconnectTimer =
        Timer(Duration(milliseconds: delayMs), _tryReconnect);
    state = AsyncData(cur.copyWith(reconnectAttempt: attempt + 1));
  }

  Future<void> _tryReconnect() async {
    if (_reconnectInFlight) {
      return;
    }
    _reconnectInFlight = true;
    final ble = ref.read(bleClientProvider);
    try {
      final attempt = state.value?.reconnectAttempt ?? 0;
      // Tier 3: if soft reconnects have failed enough times, soft-cycle
      // the slot before the next connect. fbp.connect() returning
      // success on a dead slot is the documented gatts_if-leak
      // fingerprint that "force-stop fixes it" reports map onto.
      if (attempt >= _cycleAdapterAttempt) {
        await ble.cycleAdapter(_deviceId);
      }
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
      final canaryBytes = await ble.readSection(_deviceId, 'lamp');
      if (canaryBytes.isEmpty) {
        state = AsyncError(
            const LampAuthRequiredException(), StackTrace.current);
        return;
      }
      // Canary succeeded → the link is fully usable (GATT connected
      // AND services discovered AND auth restored). NOW it's safe to
      // flip the UI to connected and push pending local state.
      //
      // Bail if disposed during the canary await — touching state would
      // throw. The notifier dispose path already cancels everything we
      // care about.
      if (!ref.mounted) return;
      _reconnectTimer?.cancel();
      final cur = state.value;
      if (cur != null) {
        state = AsyncData(cur.copyWith(connected: true, reconnectAttempt: 0));
        _pushLocalState(cur);
      }
    } catch (_) {
      // Bail if the notifier was disposed while we were awaiting — touching
      // `state` after dispose throws. This happens in tests that
      // dispose the ProviderContainer while a reconnect is mid-flight,
      // and could also happen in production if the user navigates
      // away (lamp picker → other lamp) during a slow reconnect.
      if (!ref.mounted) {
        _reconnectInFlight = false;
        return;
      }
      // Clear the flag BEFORE scheduling the next attempt — the timer
      // is scheduled via _scheduleReconnect and its `_tryReconnect`
      // call will check `_reconnectInFlight` on entry. If finally
      // hadn't yet run, that next call would incorrectly skip itself
      // and the ladder would stall on its current attempt count.
      _reconnectInFlight = false;
      _scheduleReconnect();
    } finally {
      // Defense-in-depth: the catch branches above clear the flag
      // explicitly; finally covers the success path (and any future
      // catch branch that forgets to).
      _reconnectInFlight = false;
    }
  }

  void _pushLocalState(ControlState cur) {
    _brightnessWriter
        ?.schedule(Uint8List.fromList([cur.lamp.brightness]));
    _shadeColorsWriter?.schedule(_encodeColors(cur.shade.colors));
    _baseColorsWriter?.schedule(_encodeColors(cur.base.colors));
  }
}
