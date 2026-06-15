import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/commit_section.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../../_support/seed.dart';

const _devId = 'lamp-a';
const _devIdB = 'lamp-b';

Future<void> _seed(InMemoryBleClient ble, {String deviceId = _devId}) =>
    seedControlBle(ble, deviceId: deviceId, brightness: 50);

void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  group('controlNotifier.commit(section)', () {
    test('writes a single byte to CHAR_COMMIT when hasCommitChar=true',
        () async {
      final ble = InMemoryBleClient();
      await _seed(ble);
      // Simulate Phase A firmware — CHAR_COMMIT is discoverable.
      ble.setDiscoveredChars(_devId, [BleUuids.commit]);

      final c = ProviderContainer(
        overrides: [bleClientProvider.overrideWithValue(ble)],
      );
      addTearDown(c.dispose);

      await c.read(inventoryNotifierProvider.future);
      await c
          .read(inventoryNotifierProvider.notifier)
          .add(const InventoryLamp(
            id: _devId,
            name: 'test-lamp',
            controlPassword: '',
          ));

      // Keep a listener alive so the provider stays connected.
      final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
      addTearDown(sub.close);

      await c.read(controlNotifierProvider(_devId).future);

      final n = c.read(controlNotifierProvider(_devId).notifier);
      await n.commit(CommitSection.lamp);

      final writes = ble.writesTo(_devId, BleUuids.commit);
      expect(writes.length, 1);
      expect(writes.first.length, lessThanOrEqualTo(4));
    });

    test('no-ops when hasCommitChar=false', () async {
      final ble = InMemoryBleClient();
      await _seed(ble, deviceId: _devIdB);
      // No setDiscoveredChars → probeHasCommitChar returns false.

      final c = ProviderContainer(
        overrides: [bleClientProvider.overrideWithValue(ble)],
      );
      addTearDown(c.dispose);

      await c.read(inventoryNotifierProvider.future);
      await c
          .read(inventoryNotifierProvider.notifier)
          .add(const InventoryLamp(
            id: _devIdB,
            name: 'legacy-lamp',
            controlPassword: '',
          ));

      final sub = c.listen(controlNotifierProvider(_devIdB), (_, _) {});
      addTearDown(sub.close);

      await c.read(controlNotifierProvider(_devIdB).future);

      final n = c.read(controlNotifierProvider(_devIdB).notifier);
      await n.commit(CommitSection.lamp);

      expect(ble.writesTo(_devIdB, BleUuids.commit), isEmpty);
    });
  });
}
