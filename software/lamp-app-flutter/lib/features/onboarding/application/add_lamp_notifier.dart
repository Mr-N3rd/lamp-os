import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/setup_client.dart';
import '../../inventory/application/active_lamp_notifier.dart';
import '../../inventory/application/inventory_notifier.dart';
import '../../inventory/domain/inventory_lamp.dart';
import '../domain/add_lamp_state.dart';

part 'add_lamp_notifier.g.dart';

@Riverpod(keepAlive: true, name: 'addLampNotifierProvider')
class AddLampNotifier extends _$AddLampNotifier {
  @override
  AddLampState build() => const AddLampState();

  Future<void> select(String deviceId) async {
    final ble = ref.read(bleClientProvider);
    await ble.connect(deviceId);
    state = state.copyWith(
      deviceId: deviceId,
      step: AddLampStep.name,
    );
  }

  void setName(String n) => state = state.copyWith(name: n);
  void setPassword(String p) => state = state.copyWith(password: p);

  void next() {
    state = state.copyWith(step: switch (state.step) {
      AddLampStep.scan => AddLampStep.name,
      AddLampStep.name => AddLampStep.password,
      AddLampStep.password => AddLampStep.done,
      AddLampStep.done => AddLampStep.done,
    });
  }

  void previous() {
    state = state.copyWith(step: switch (state.step) {
      AddLampStep.scan => AddLampStep.scan,
      AddLampStep.name => AddLampStep.scan,
      AddLampStep.password => AddLampStep.name,
      AddLampStep.done => AddLampStep.password,
    });
  }

  Future<void> submit() async {
    state = state.copyWith(status: AddLampStatus.working, errorMessage: null);
    try {
      final ble = ref.read(bleClientProvider);
      await SetupClient(ble: ble).claim(
        deviceId: state.deviceId,
        name: state.name,
        password: state.password,
      );
      await ref.read(inventoryNotifierProvider.notifier).add(
            InventoryLamp(
              id: state.deviceId,
              name: state.name,
              controlPassword: state.password,
            ),
          );
      await ref
          .read(activeLampNotifierProvider.notifier)
          .set(state.deviceId);
      state = state.copyWith(
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

  Future<void> add({
    required String deviceId,
    required String name,
  }) async {
    state = state.copyWith(status: AddLampStatus.working, errorMessage: null);
    try {
      await ref.read(inventoryNotifierProvider.notifier).add(
            InventoryLamp(id: deviceId, name: name),
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
