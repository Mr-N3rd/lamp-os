import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:lamp_app/features/social/domain/social_mode.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../../_support/seed.dart';

const _devIdA = 'lamp-a'; // Phase A (hasCommitChar=true)
const _devIdB = 'lamp-b'; // pre-Phase-A (hasCommitChar=false)

/// Build a container for a Phase A lamp (CHAR_COMMIT discoverable).
Future<({ProviderContainer container, InMemoryBleClient ble})>
    _buildPhaseA() async {
  final ble = InMemoryBleClient();
  await seedControlBle(ble, deviceId: _devIdA, brightness: 50);
  ble.setDiscoveredChars(_devIdA, [BleUuids.commit]);

  final c = ProviderContainer(
    overrides: [bleClientProvider.overrideWithValue(ble)],
  );

  await c.read(inventoryNotifierProvider.future);
  await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
        id: _devIdA,
        name: 'original-name',
        controlPassword: '',
      ));

  c.listen(controlNotifierProvider(_devIdA), (_, _) {});
  await c.read(controlNotifierProvider(_devIdA).future);

  return (container: c, ble: ble);
}

/// Build a container for a pre-Phase-A lamp (no CHAR_COMMIT).
Future<({ProviderContainer container, InMemoryBleClient ble})>
    _buildPrePhaseA() async {
  final ble = InMemoryBleClient();
  await seedControlBle(ble, deviceId: _devIdB, brightness: 50);
  // No setDiscoveredChars → probeHasCommitChar returns false.

  final c = ProviderContainer(
    overrides: [bleClientProvider.overrideWithValue(ble)],
  );

  await c.read(inventoryNotifierProvider.future);
  await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
        id: _devIdB,
        name: 'original-name',
        controlPassword: '',
      ));

  c.listen(controlNotifierProvider(_devIdB), (_, _) {});
  await c.read(controlNotifierProvider(_devIdB).future);

  return (container: c, ble: ble);
}

void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  // -------------------------------------------------------------------------
  // setLampName
  // -------------------------------------------------------------------------

  group('setLampName phase-aware behavior', () {
    test('Phase A: writeSettingsBlob fires + inventory cache updates', () async {
      final (:container, :ble) = await _buildPhaseA();
      addTearDown(container.dispose);

      final notifier =
          container.read(controlNotifierProvider(_devIdA).notifier);
      await notifier.setLampName('newname');

      // BLE write to settingsBlob occurred exactly once.
      final writes = ble.writesTo(_devIdA, BleUuids.settingsBlob);
      expect(writes.length, 1,
          reason: 'Phase A lamp should write settings_blob on rename');

      // Inventory cache reflects the new name.
      final entries = await container.read(inventoryNotifierProvider.future);
      expect(
        entries.firstWhere((l) => l.id == _devIdA).name,
        'newname',
        reason: 'inventory cache must update so AppBar LampChip is current',
      );

      // In-memory state also updated.
      final st = container.read(controlNotifierProvider(_devIdA)).value!;
      expect(st.lamp.name, 'newname');
    });

    test('Pre-Phase-A: no BLE write but state + inventory still update',
        () async {
      final (:container, :ble) = await _buildPrePhaseA();
      addTearDown(container.dispose);

      final notifier =
          container.read(controlNotifierProvider(_devIdB).notifier);
      await notifier.setLampName('newname');

      // No settingsBlob write for pre-Phase-A.
      expect(
        ble.writesTo(_devIdB, BleUuids.settingsBlob),
        isEmpty,
        reason: 'pre-Phase-A lamp must NOT write settings_blob',
      );

      // Inventory cache still updated (B.7 spec: always).
      final entries = await container.read(inventoryNotifierProvider.future);
      expect(
        entries.firstWhere((l) => l.id == _devIdB).name,
        'newname',
        reason: 'inventory cache must update even on pre-Phase-A lamps',
      );

      // In-memory state updated.
      final st = container.read(controlNotifierProvider(_devIdB)).value!;
      expect(st.lamp.name, 'newname');
    });
  });

  // -------------------------------------------------------------------------
  // setLampAdvancedEnabled
  // -------------------------------------------------------------------------

  group('setLampAdvancedEnabled phase-aware behavior', () {
    test('Phase A: writeSettingsBlob fires', () async {
      final (:container, :ble) = await _buildPhaseA();
      addTearDown(container.dispose);

      final notifier =
          container.read(controlNotifierProvider(_devIdA).notifier);
      await notifier.setLampAdvancedEnabled(true);

      expect(
        ble.writesTo(_devIdA, BleUuids.settingsBlob).length,
        1,
        reason: 'Phase A lamp should write settings_blob on advancedEnabled',
      );

      final st = container.read(controlNotifierProvider(_devIdA)).value!;
      expect(st.lamp.advancedEnabled, isTrue);
    });

    test('Pre-Phase-A: no BLE write but state still updates', () async {
      final (:container, :ble) = await _buildPrePhaseA();
      addTearDown(container.dispose);

      final notifier =
          container.read(controlNotifierProvider(_devIdB).notifier);
      await notifier.setLampAdvancedEnabled(true);

      expect(
        ble.writesTo(_devIdB, BleUuids.settingsBlob),
        isEmpty,
        reason: 'pre-Phase-A lamp must NOT write settings_blob',
      );

      final st = container.read(controlNotifierProvider(_devIdB)).value!;
      expect(st.lamp.advancedEnabled, isTrue);
    });
  });

  // -------------------------------------------------------------------------
  // setLampSocialMode
  // -------------------------------------------------------------------------

  group('setLampSocialMode phase-aware behavior', () {
    test('Phase A: writeSettingsBlob fires', () async {
      final (:container, :ble) = await _buildPhaseA();
      addTearDown(container.dispose);

      final notifier =
          container.read(controlNotifierProvider(_devIdA).notifier);
      await notifier.setLampSocialMode(SocialMode.extrovert);

      expect(
        ble.writesTo(_devIdA, BleUuids.settingsBlob).length,
        1,
        reason: 'Phase A lamp should write settings_blob on socialMode change',
      );

      final st = container.read(controlNotifierProvider(_devIdA)).value!;
      expect(st.lamp.socialMode, SocialMode.extrovert);
    });

    test('Pre-Phase-A: no BLE write but state still updates', () async {
      final (:container, :ble) = await _buildPrePhaseA();
      addTearDown(container.dispose);

      final notifier =
          container.read(controlNotifierProvider(_devIdB).notifier);
      await notifier.setLampSocialMode(SocialMode.introvert);

      expect(
        ble.writesTo(_devIdB, BleUuids.settingsBlob),
        isEmpty,
        reason: 'pre-Phase-A lamp must NOT write settings_blob',
      );

      final st = container.read(controlNotifierProvider(_devIdB)).value!;
      expect(st.lamp.socialMode, SocialMode.introvert);
    });
  });
}
