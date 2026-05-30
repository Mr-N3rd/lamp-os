import 'dart:convert';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/setup_client.dart';
import '../../../core/ble/uuids.dart';
import '../../control/application/auth_client.dart';
import '../../inventory/application/active_lamp_notifier.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../../inventory/domain/inventory_lamp.dart';
import '../domain/add_lamp_state.dart';

part 'add_lamp_notifier.g.dart';

/// Pick a 1..8 critter index. Each lamp keeps its own random pick across
/// the connecting view and the lamp preview so users associate the lamp
/// with a consistent little friend.
int _pickCritterIndex() => Random().nextInt(8) + 1;

@Riverpod(keepAlive: true, name: 'addLampNotifierProvider')
class AddLampNotifier extends _$AddLampNotifier {
  /// How long to wait after the firmware applies setup (reboots) before we
  /// attempt to reconnect for the post-claim password-verification probe.
  /// Tests can override this to `Duration.zero`.
  @visibleForTesting
  static Duration verifyDelay = const Duration(seconds: 5);

  @override
  AddLampState build() => const AddLampState();

  Future<void> select(String deviceId) async {
    // Phase 1: flip state synchronously so the shell can render a
    // loading view as soon as the picker pushes the route — the user
    // shouldn't see a blank pane while ble.connect awaits.
    state = state.copyWith(
      deviceId: deviceId,
      step: AddLampStep.connecting,
      status: AddLampStatus.working,
    );
    // Phase 2: actually connect (slow over BLE).
    final ble = ref.read(bleClientProvider);
    await ble.connect(deviceId);
    // Phase 3: advance to the name form.
    state = state.copyWith(
      step: AddLampStep.name,
      status: AddLampStatus.idle,
    );
  }

  void setName(String n) => state = state.copyWith(name: n);
  void setPassword(String p) => state = state.copyWith(password: p);

  void next() {
    state = state.copyWith(step: switch (state.step) {
      AddLampStep.scan => AddLampStep.connecting,
      AddLampStep.connecting => AddLampStep.name,
      AddLampStep.name => AddLampStep.password,
      AddLampStep.password => AddLampStep.verifying,
      AddLampStep.verifying => AddLampStep.done,
      AddLampStep.done => AddLampStep.done,
    });
  }

  void previous() {
    state = state.copyWith(step: switch (state.step) {
      AddLampStep.scan => AddLampStep.scan,
      AddLampStep.connecting => AddLampStep.scan,
      AddLampStep.name => AddLampStep.scan,
      AddLampStep.password => AddLampStep.name,
      AddLampStep.verifying => AddLampStep.password,
      AddLampStep.done => AddLampStep.password,
    });
  }

  Future<void> submit() async {
    state = state.copyWith(
      status: AddLampStatus.working,
      error: AddLampError.none,
      errorMessage: null,
    );
    final ble = ref.read(bleClientProvider);

    // Step 0: ensure the link is up with a FRESH handle. select()
    // connected ~30s ago; by now Android has dropped the link via
    // LINK_SUPERVISION_TIMEOUT, but flutter_blue_plus's cached state
    // may still report "connected", so a plain connect() is a no-op
    // and the next write fails with fbp-code 6 (deviceIsDisconnected).
    // Force a real reconnect via disconnect → 500ms pause → connect
    // (same shape as the post-reboot reconnect below).
    try {
      try {
        await ble.disconnect(state.deviceId);
      } catch (_) {
        // already-disconnected is fine
      }
      await Future<void>.delayed(const Duration(milliseconds: 500));
      await ble.connect(state.deviceId);
    } catch (e) {
      state = state.copyWith(
        status: AddLampStatus.error,
        error: AddLampError.connectFailed,
        errorMessage: e.toString(),
      );
      return;
    }

    // Step 1: claim. Failures here are usually BLE-side (connect dropped,
    // setup characteristics rejected) — surface as claimFailed and bail.
    try {
      await SetupClient(ble: ble).claim(
        deviceId: state.deviceId,
        name: state.name,
        password: state.password,
      );
    } catch (e) {
      state = state.copyWith(
        status: AddLampStatus.error,
        error: AddLampError.claimFailed,
        errorMessage: e.toString(),
      );
      return;
    }

    // Step 2: wait for the lamp to reboot, then reconnect + authenticate
    // + probe a section read to confirm the password stuck. The lampSection
    // characteristic is gated by auth; an unauthenticated read returns
    // empty bytes (or fails to decode) which we treat as a password mismatch.
    state = state.copyWith(
      step: AddLampStep.verifying,
      status: AddLampStatus.working,
    );
    try {
      await Future<void>.delayed(verifyDelay);
      // Force a fresh BLE link. After setupApply the firmware fades + reboots,
      // but flutter_blue_plus typically still believes it's connected (it only
      // notices via LINK_SUPERVISION_TIMEOUT, which can take >1s). Calling
      // connect() in that stale state is a no-op, so the next write fires
      // into a dead handle and Android returns GATT_ERROR (133). Explicitly
      // disconnecting first guarantees a real reconnect against the rebooted
      // lamp.
      try {
        await ble.disconnect(state.deviceId);
      } catch (_) {
        // already-disconnected is fine
      }
      await Future<void>.delayed(const Duration(milliseconds: 500));
      await ble.connect(state.deviceId);
      await AuthClient(ble: ble).authenticate(
        deviceId: state.deviceId,
        password: state.password,
      );
      final bytes = await ble.read(
        state.deviceId,
        BleUuids.controlService,
        BleUuids.lampSection,
      );
      if (bytes.isEmpty) {
        throw const FormatException('auth-rejected');
      }
      final j = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
      if (j['name'] == null) {
        throw const FormatException('auth-rejected');
      }
    } on FormatException catch (_) {
      state = state.copyWith(
        status: AddLampStatus.error,
        step: AddLampStep.password,
        error: AddLampError.wrongPassword,
        errorMessage: "Wrong password — the lamp did not accept it.",
      );
      return;
    } catch (e) {
      state = state.copyWith(
        status: AddLampStatus.error,
        step: AddLampStep.password,
        error: AddLampError.connectFailed,
        errorMessage: e.toString(),
      );
      return;
    }

    // Step 3: success — persist and advance.
    await ref.read(inventoryNotifierProvider.notifier).add(
          InventoryLamp(
            id: state.deviceId,
            name: state.name,
            controlPassword: state.password,
            critterIndex: _pickCritterIndex(),
          ),
        );
    await ref
        .read(activeLampNotifierProvider.notifier)
        .set(state.deviceId);
    state = state.copyWith(
      step: AddLampStep.done,
      status: AddLampStatus.idle,
    );
  }

  Future<void> add({
    required String deviceId,
    required String name,
  }) async {
    state = state.copyWith(status: AddLampStatus.working, errorMessage: null);
    try {
      await ref.read(inventoryNotifierProvider.notifier).add(
            InventoryLamp(
              id: deviceId,
              name: name,
              critterIndex: _pickCritterIndex(),
            ),
          );
      await ref.read(activeLampNotifierProvider.notifier).set(deviceId);
      state = state.copyWith(
        deviceId: deviceId,
        name: name,
        step: AddLampStep.done,
        status: AddLampStatus.idle,
      );
    } catch (e) {
      state = state.copyWith(
        status: AddLampStatus.error,
        errorMessage: e.toString(),
      );
    }
  }

  void reset() => state = const AddLampState();
}
