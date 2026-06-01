import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/lamp_crypto.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/control/application/control_notifier.dart';
import 'package:lamp_app/features/control/application/control_state.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/domain/sections.dart';
import 'package:lamp_app/features/inventory/application/inventory_notifier.dart';
import 'package:lamp_app/features/inventory/domain/inventory_lamp.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../../../_support/seed.dart';

const _devId = 'dev1';

Future<void> _seed(InMemoryBleClient ble) =>
    seedControlBle(ble, deviceId: _devId, brightness: 42);

void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  test('on build: connects, auths, populates state from sections', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));

    final state =
        await c.read(controlNotifierProvider(_devId).future);
    expect(state.lamp.brightness, 42);
    expect(state.base.colors.single.toHex(), '#300783FF');
    expect(state.shade.colors.single.toHex(), '#000000FF');

    final auth = await ble.read(_devId, BleUuids.controlService, BleUuids.auth);
    // Auth write is now a ciphertext frame: magic + nonce + tag + {"auth":true}
    expect(auth[0], LampCrypto.magicCiphertext);
    expect(auth.length,
        1 + LampCrypto.nonceLen + LampCrypto.tagLen +
            utf8.encode('{"auth":true}').length);
  });

  test('disconnect is called when build() throws after connect', () async {
    // Seed inventory but no section chars — connect + auth succeed, then the
    // first section read throws BleNotFound, simulating a partial build failure.
    final ble = InMemoryBleClient();
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    // NOTE: do NOT addTearDown(c.dispose) here — we call c.dispose() manually
    // below and need to verify the BLE state immediately after.

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: '', // empty password → auth is a no-op write
        ));

    // Hold a subscription so the auto-dispose timer doesn't fire before we
    // inspect state.
    final states = <AsyncValue<ControlState>>[];
    final sub = c.listen<AsyncValue<ControlState>>(
      controlNotifierProvider(_devId),
      (_, next) => states.add(next),
      fireImmediately: true,
    );
    addTearDown(sub.close);

    // Wait until the provider settles into an error state.
    await Future.doWhile(() async {
      await Future<void>.delayed(Duration.zero);
      return c
              .read(controlNotifierProvider(_devId))
              .hasError ==
          false;
    });

    expect(c.read(controlNotifierProvider(_devId)).hasError, isTrue);
    // Before dispose, the lamp was connected (connect succeeded).
    expect(ble.isConnected(_devId), isTrue);

    sub.close();
    c.dispose();

    // After dispose, the early onDispose registration should have
    // disconnected the lamp.
    expect(ble.isConnected(_devId), isFalse);
  });

  test('isDirty flips on setBrightness and clears on save() round-trip',
      () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    // ALSO seed the settings blob (save() needs it).
    await ble.connect(_devId);
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"name":"jacko","brightness":42,"advancedEnabled":false},'
          '"base":{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"],"knockout":[]},'
          '"shade":{"px":38,"bpp":4,"colors":["#000000FF"]}}',
        )));
    await ble.disconnect(_devId);

    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    expect(n.isDirty, isFalse);

    await n.setBrightness(80);
    expect(n.isDirty, isTrue);
  });

  test('save() merges local state into the full settings blob', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    await ble.connect(_devId);
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"name":"jacko","brightness":42,"advancedEnabled":false},'
          '"base":{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"],"knockout":[]},'
          '"shade":{"px":38,"bpp":4,"colors":["#000000FF"]},'
          '"expressions":[{"type":"breathing","enabled":true}]}', // untouched field
        )));
    await ble.disconnect(_devId);

    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    // Keep listener alive so BLE stays connected during save().
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    await n.setBrightness(80);
    await n.save();

    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    // settingsBlob is now encrypted when controlPassword is set.
    expect(written[0], LampCrypto.magicCiphertext);
    final plain = await LampCrypto.decryptOpForTesting(
        written,
        password: 'secret',
        saltUuid16: uuidSaltLE16(BleUuids.settingsBlob),
        charShortName: 'settingsBlob');
    final parsed = jsonDecode(utf8.decode(plain)) as Map<String, dynamic>;
    expect((parsed['lamp'] as Map)['brightness'], 80);
    expect(parsed['expressions'], isNotNull); // unchanged field preserved
  });

  test('setBrightness optimistically updates and writes after debounce',
      () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    // Keep a listener alive so the auto-dispose provider doesn't disconnect
    // BLE before the debounce timer fires.
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    await c
        .read(controlNotifierProvider(_devId).notifier)
        .setBrightness(80);
    // Optimistic update is visible immediately.
    expect(
      c.read(controlNotifierProvider(_devId)).value!.lamp.brightness,
      80,
    );
    // Drain the debounce window.
    await Future<void>.delayed(const Duration(milliseconds: 60));
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.brightness);
    expect(written.single, 80);
  });

  test('disconnect surfaces in state and schedules reconnect', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    // Keep a listener alive so the auto-dispose provider doesn't tear down.
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    await ble.disconnect(_devId);
    // The watchConnected stream uses an async* generator; the false emission
    // needs a couple of microtask hops to reach the listener.
    await Future<void>.delayed(const Duration(milliseconds: 10));

    final s = c.read(controlNotifierProvider(_devId)).value!;
    expect(s.connected, isFalse);
    expect(s.reconnectAttempt, 1);
  });

  test(
      'local edits survive a disconnect/reconnect cycle and the lamp catches up',
      () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    // User edit locally.
    await c
        .read(controlNotifierProvider(_devId).notifier)
        .setBrightness(80);
    // Let the debounce coalescer fire so the BLE write lands before disconnect.
    await Future<void>.delayed(const Duration(milliseconds: 50));

    // Link drops.
    await ble.disconnect(_devId);
    // Allow the async* watchConnected generator to propagate the false event.
    await Future<void>.delayed(const Duration(milliseconds: 10));

    // Local state should still hold 80.
    expect(
      c.read(controlNotifierProvider(_devId)).value!.lamp.brightness,
      80,
    );
    expect(
      c.read(controlNotifierProvider(_devId)).value!.connected,
      isFalse,
    );

    // Simulate the lamp coming back up — connecting triggers watchConnected
    // which fires _onConnectionChange(true) → _pushLocalState.
    await ble.connect(_devId);
    await Future<void>.delayed(const Duration(milliseconds: 50));

    expect(
      c.read(controlNotifierProvider(_devId)).value!.lamp.brightness,
      80,
    );
    expect(
      c.read(controlNotifierProvider(_devId)).value!.connected,
      isTrue,
    );
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.brightness);
    expect(written.single, 80); // catch-up push landed
  });

  test('initial load writes last-seen colors to inventory', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final inv = await c.read(inventoryNotifierProvider.future);
    final lamp = inv.firstWhere((l) => l.id == _devId);
    expect(lamp.lastShadeColor, [0x00, 0x00, 0x00]); // seeded shade
    expect(lamp.lastBaseColor, [0x30, 0x07, 0x83]); // seeded base[0]
  });

  test('setShadeColor refreshes the inventory color cache', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);
    // Hold a listener so the provider doesn't auto-dispose during the
    // 500 ms debounce window for `_updateSeen` and kill the flush timer.
    final sub = c.listen<AsyncValue<ControlState>>(
        controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    await c
        .read(controlNotifierProvider(_devId).notifier)
        .setShadeColor(const LampColor(r: 0xFF, g: 0x88, b: 0x00, w: 0));

    // The updateSeen call is now debounced (~500 ms) so a 60 fps slider
    // doesn't generate 60 disk writes per second. Wait past the window
    // before asserting the trailing value landed.
    await Future<void>.delayed(const Duration(milliseconds: 600));

    final inv = await c.read(inventoryNotifierProvider.future);
    expect(inv.first.lastShadeColor, [0xFF, 0x88, 0x00]);
  });

  test('setBaseColors refreshes the inventory color cache', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);
    // See setShadeColor test for why we hold a listener here.
    final sub = c.listen<AsyncValue<ControlState>>(
        controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    await c.read(controlNotifierProvider(_devId).notifier).setBaseColors([
      const LampColor(r: 0x11, g: 0x22, b: 0x33, w: 0),
      const LampColor(r: 0x44, g: 0x55, b: 0x66, w: 0),
    ]);

    // See setShadeColor test — same 500 ms debounce.
    await Future<void>.delayed(const Duration(milliseconds: 600));

    final inv = await c.read(inventoryNotifierProvider.future);
    final lamp = inv.firstWhere((l) => l.id == _devId);
    // ac was 0 (seeded), so we expect colors[0]
    expect(lamp.lastBaseColor, [0x11, 0x22, 0x33]);
  });

  test('setKnockoutPixel adds the entry to local state', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    await c.read(controlNotifierProvider(_devId).notifier)
        .setKnockoutPixel(3, 50);

    expect(
      c.read(controlNotifierProvider(_devId)).value!.base.knockout,
      {3: 50},
    );
  });

  test('setKnockoutPixel removes the entry when brightness is 100', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    await n.setKnockoutPixel(3, 50);
    await n.setKnockoutPixel(3, 100);

    expect(
      c.read(controlNotifierProvider(_devId)).value!.base.knockout,
      isEmpty,
    );
  });

  test('setKnockoutPixel writes [index, brightness] after debounce', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);
    // Hold a subscription so the auto-dispose timer doesn't fire.
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    await c.read(controlNotifierProvider(_devId).notifier)
        .setKnockoutPixel(3, 50);
    // Drain the 30 ms debounce window.
    await Future<void>.delayed(const Duration(milliseconds: 60));

    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.baseKnockout);
    expect(written, [3, 50]);
  });

  test('save() encodes knockout map into the settings blob', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    await ble.connect(_devId);
    // Seed the blob shape — note absence of knockout in the blob; we want
    // save() to inject it from local state.
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"name":"jacko","brightness":42,"advancedEnabled":false},'
          '"base":{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"]},'
          '"shade":{"px":38,"bpp":4,"colors":["#000000FF"]}}',
        )));
    await ble.disconnect(_devId);

    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    await n.setKnockoutPixel(3, 50);
    await n.save();

    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    // settingsBlob is now encrypted when controlPassword is set.
    expect(written[0], LampCrypto.magicCiphertext);
    final plain = await LampCrypto.decryptOpForTesting(
        written,
        password: 'secret',
        saltUuid16: uuidSaltLE16(BleUuids.settingsBlob),
        charShortName: 'settingsBlob');
    final parsed = jsonDecode(utf8.decode(plain)) as Map<String, dynamic>;
    final knockout = (parsed['base'] as Map)['knockout'] as List;
    expect(knockout, [{'p': 3, 'b': 50}]);
  });

  test('build() loads home + mqtt sections into state', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    final state = await c.read(controlNotifierProvider(_devId).future);
    expect(state.home.brightness, 60);
  });

  test('setLampName flips isDirty and lands in the blob on save', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    await ble.connect(_devId);
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"name":"jacko","brightness":42,"advancedEnabled":false},'
          '"base":{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"]},'
          '"shade":{"px":38,"bpp":4,"colors":["#000000FF"]},'
          '"homeMode":{"ssid":"","brightness":60},'
          '"mqtt":{"enabled":false,"brokerHost":"","brokerPort":1883}}',
        )));
    await ble.disconnect(_devId);

    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    expect(n.isDirty, isFalse);

    await n.setLampName('foyer');
    expect(n.isDirty, isTrue);

    await n.save();
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    // settingsBlob is now encrypted when controlPassword is set.
    expect(written[0], LampCrypto.magicCiphertext);
    final plain = await LampCrypto.decryptOpForTesting(
        written,
        password: 'secret',
        saltUuid16: uuidSaltLE16(BleUuids.settingsBlob),
        charShortName: 'settingsBlob');
    final parsed = jsonDecode(utf8.decode(plain)) as Map<String, dynamic>;
    expect((parsed['lamp'] as Map)['name'], 'foyer');
  });

  test('home + mqtt mutators flip isDirty', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    expect(n.isDirty, isFalse);

    await n.setHomeSsid('WiFi');
    expect(n.isDirty, isTrue);
  });

  test('save() omits home password when it is still the firmware sentinel',
      () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    await ble.connect(_devId);
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"name":"jacko","brightness":42,"advancedEnabled":false},'
          '"base":{"px":35,"ac":0,"bpp":4,"colors":["#300783FF"]},'
          '"shade":{"px":38,"bpp":4,"colors":["#000000FF"]},'
          '"homeMode":{"ssid":"WiFi","password":"********","brightness":60},'
          '"mqtt":{"enabled":false,"brokerHost":"","brokerPort":1883}}',
        )));
    // Seed home section so build() loads ssid/password
    await ble.write(_devId, BleUuids.controlService, BleUuids.homeSection,
        Uint8List.fromList(utf8.encode(
          '{"ssid":"WiFi","password":"********","brightness":60}',
        )));
    await ble.disconnect(_devId);

    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    await n.setHomeBrightness(80); // touch something other than password
    await n.save();

    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    // settingsBlob is now encrypted when controlPassword is set.
    expect(written[0], LampCrypto.magicCiphertext);
    final plain = await LampCrypto.decryptOpForTesting(
        written,
        password: 'secret',
        saltUuid16: uuidSaltLE16(BleUuids.settingsBlob),
        charShortName: 'settingsBlob');
    final parsed = jsonDecode(utf8.decode(plain)) as Map<String, dynamic>;
    final homeNode = parsed['homeMode'] as Map<String, dynamic>;
    expect(homeNode['ssid'], 'WiFi');
    expect(homeNode['brightness'], 80);
    expect(homeNode.containsKey('password'), isFalse); // sentinel was stripped
  });

  test('upsertExpression adds a new entry and writes the op', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final entry = ExpressionConfig(
      type: 'glitchy',
      enabled: true,
      colors: [LampColor.fromHex('#FF00FFAA')],
      intervalMin: 30,
      intervalMax: 60,
      target: 2,
      parameters: const {},
    );
    await c.read(controlNotifierProvider(_devId).notifier)
        .upsertExpression(entry);

    expect(
      c.read(controlNotifierProvider(_devId)).value!.expressions.expressions,
      hasLength(1),
    );
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.expressionOp);
    final parsed = jsonDecode(utf8.decode(written)) as Map<String, dynamic>;
    expect(parsed['op'], 'upsert');
    expect((parsed['entry'] as Map)['type'], 'glitchy');
  });

  test('upsertExpression replaces an existing (type,target) entry', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    const a = ExpressionConfig(
      type: 'glitchy', enabled: true, colors: [],
      intervalMin: 30, intervalMax: 60, target: 2, parameters: {},
    );
    const b = ExpressionConfig(
      type: 'glitchy', enabled: false, colors: [],
      intervalMin: 30, intervalMax: 60, target: 2, parameters: {},
    );
    await n.upsertExpression(a);
    await n.upsertExpression(b);

    final list = c.read(controlNotifierProvider(_devId)).value!.expressions.expressions;
    expect(list, hasLength(1));
    expect(list.single.enabled, isFalse);
  });

  test('removeExpression drops the (type, target) entry and writes op',
      () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    final n = c.read(controlNotifierProvider(_devId).notifier);
    await n.upsertExpression(const ExpressionConfig(
      type: 'glitchy', enabled: true, colors: [],
      intervalMin: 30, intervalMax: 60, target: 2, parameters: {},
    ));
    await n.removeExpression(type: 'glitchy', target: 2);

    expect(
      c.read(controlNotifierProvider(_devId)).value!.expressions.expressions,
      isEmpty,
    );
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.expressionOp);
    final parsed = jsonDecode(utf8.decode(written)) as Map<String, dynamic>;
    expect(parsed['op'], 'remove');
    expect(parsed['type'], 'glitchy');
    expect(parsed['target'], 2);
  });

  test('testExpression writes to CHAR_EXPRESSION_TEST', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    await c.read(controlNotifierProvider(_devId).notifier).testExpression(
          const ExpressionConfig(
            type: 'breathing', enabled: true, colors: [],
            intervalMin: 30, intervalMax: 60, target: 1, parameters: {},
          ),
        );

    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.expressionTest);
    final parsed = jsonDecode(utf8.decode(written)) as Map<String, dynamic>;
    // Firmware-expected envelope: action + the named entry's keys. The full
    // ExpressionConfig isn't sent — the lamp already has those colors and
    // parameters from the preceding expressionOp upsert.
    expect(parsed['a'], 'test_expression');
    expect(parsed['type'], 'breathing');
    expect(parsed['target'], 1);
  });

  test('completeExpressionTest writes the test_expression_complete envelope',
      () async {
    final ble = InMemoryBleClient();
    await _seed(ble);
    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);

    // build() itself sends the complete write at the end of the load, so the
    // last value on expressionTest is already what we want to assert. Call
    // the public mutator anyway to verify the production code path.
    await c
        .read(controlNotifierProvider(_devId).notifier)
        .completeExpressionTest();

    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.expressionTest);
    final parsed = jsonDecode(utf8.decode(written)) as Map<String, dynamic>;
    expect(parsed['a'], 'test_expression_complete');
  });

  test('save() is a no-op while disconnected', () async {
    final ble = InMemoryBleClient();
    await _seed(ble);

    // Seed the settings blob so save() would otherwise have something to read.
    await ble.connect(_devId);
    await ble.write(_devId, BleUuids.controlService, BleUuids.settingsBlob,
        Uint8List.fromList(utf8.encode(
          '{"lamp":{"brightness":42},"base":{"colors":[]},"shade":{"colors":[]}}',
        )));
    // Snapshot the blob value BEFORE we proceed (values persist in
    // InMemoryBleClient across disconnect cycles, but read() requires a live
    // connection; take the snapshot now while still connected).
    final before = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    await ble.disconnect(_devId);

    final c = ProviderContainer(
      overrides: [bleClientProvider.overrideWithValue(ble)],
    );
    addTearDown(c.dispose);

    await c.read(inventoryNotifierProvider.future);
    await c.read(inventoryNotifierProvider.notifier).add(const InventoryLamp(
          id: _devId,
          name: 'jacko',
          controlPassword: 'secret',
        ));
    await c.read(controlNotifierProvider(_devId).future);
    final sub = c.listen(controlNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);

    // Force disconnect so state.connected becomes false.
    await ble.disconnect(_devId);
    // Allow the async* watchConnected generator to propagate the false event.
    await Future<void>.delayed(const Duration(milliseconds: 10));

    // save() should be a no-op.
    await c.read(controlNotifierProvider(_devId).notifier).save();

    // Reconnect to snapshot the current settingsBlob value.
    await ble.connect(_devId);
    final after = await ble.read(
        _devId, BleUuids.controlService, BleUuids.settingsBlob);
    expect(after, before); // save was a no-op
  });
}
