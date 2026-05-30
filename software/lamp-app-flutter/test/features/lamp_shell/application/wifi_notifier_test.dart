import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:lamp_app/core/ble/ble_client.dart';
import 'package:lamp_app/core/ble/ble_client_provider.dart';
import 'package:lamp_app/core/ble/uuids.dart';
import 'package:lamp_app/features/lamp_shell/application/wifi_notifier.dart';

const _devId = 'lamp-x';

Future<ProviderContainer> _seeded(String stateJson) async {
  final ble = InMemoryBleClient();
  await ble.connect(_devId);
  await ble.write(
    _devId,
    BleUuids.controlService,
    BleUuids.wifiState,
    Uint8List.fromList(utf8.encode(stateJson)),
  );
  return ProviderContainer(
    overrides: [bleClientProvider.overrideWithValue(ble)],
  );
}

void main() {
  test('build subscribes and parses initial wifiState read', () async {
    final c = await _seeded(
        '{"state":"idle","scanResults":[{"ssid":"home","rssi":-55,"encrypted":true}]}');
    addTearDown(c.dispose);

    final w = await c.read(wifiNotifierProvider(_devId).future);
    expect(w.state, 'idle');
    expect(w.scanResults, hasLength(1));
    expect(w.scanResults.first.ssid, 'home');
    expect(w.scanResults.first.rssi, -55);
    expect(w.scanResults.first.encrypted, isTrue);
  });

  test('scan() writes {op:scan} to wifiOp', () async {
    final c = await _seeded('{"state":"idle"}');
    addTearDown(c.dispose);
    // Prime the provider so the wifiOp characteristic gets written below.
    await c.read(wifiNotifierProvider(_devId).future);

    await c.read(wifiNotifierProvider(_devId).notifier).scan();

    // Verify by reading the characteristic the notifier wrote.
    final ble = c.read(bleClientProvider);
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.wifiOp);
    final decoded = jsonDecode(utf8.decode(written));
    expect(decoded, {'op': 'scan'});
  });

  test('connect() writes {op:connect, ssid, password}', () async {
    final c = await _seeded('{"state":"idle"}');
    addTearDown(c.dispose);
    await c.read(wifiNotifierProvider(_devId).future);

    await c
        .read(wifiNotifierProvider(_devId).notifier)
        .connect('home', 'sekret');

    final ble = c.read(bleClientProvider);
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.wifiOp);
    expect(jsonDecode(utf8.decode(written)),
        {'op': 'connect', 'ssid': 'home', 'password': 'sekret'});
  });

  test('forget() writes {op:forget}', () async {
    final c = await _seeded('{"state":"idle"}');
    addTearDown(c.dispose);
    await c.read(wifiNotifierProvider(_devId).future);

    await c.read(wifiNotifierProvider(_devId).notifier).forget();

    final ble = c.read(bleClientProvider);
    final written = await ble.read(
        _devId, BleUuids.controlService, BleUuids.wifiOp);
    expect(jsonDecode(utf8.decode(written)), {'op': 'forget'});
  });

  test('notify updates state and preserves scan results', () async {
    final c = await _seeded(
        '{"state":"idle","scanResults":[{"ssid":"home","rssi":-55,"encrypted":true}]}');
    addTearDown(c.dispose);
    // Keep the provider alive so auto-dispose doesn't cancel the subscription
    // before the notify arrives.
    final sub = c.listen(wifiNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);
    await c.read(wifiNotifierProvider(_devId).future);

    // Push a notify with state "connected" and NO scanResults.
    final ble = c.read(bleClientProvider);
    await ble.write(
      _devId,
      BleUuids.controlService,
      BleUuids.wifiState,
      Uint8List.fromList(utf8.encode(
          '{"state":"connected","ssid":"home","ip":"192.168.1.20"}')),
    );
    // Let the listener fire.
    await Future<void>.delayed(Duration.zero);

    final w = c.read(wifiNotifierProvider(_devId)).value!;
    expect(w.state, 'connected');
    expect(w.ssid, 'home');
    expect(w.ip, '192.168.1.20');
    // Scan results preserved from prior payload.
    expect(w.scanResults, hasLength(1));
    expect(w.scanResults.first.ssid, 'home');
  });

  test('notify with state=failed sets lastError', () async {
    final c = await _seeded('{"state":"connecting"}');
    addTearDown(c.dispose);
    // Keep the provider alive so auto-dispose doesn't cancel the subscription
    // before the notify arrives.
    final sub = c.listen(wifiNotifierProvider(_devId), (_, _) {});
    addTearDown(sub.close);
    await c.read(wifiNotifierProvider(_devId).future);

    final ble = c.read(bleClientProvider);
    await ble.write(
      _devId,
      BleUuids.controlService,
      BleUuids.wifiState,
      Uint8List.fromList(
          utf8.encode('{"state":"failed","lastError":"auth"}')),
    );
    await Future<void>.delayed(Duration.zero);

    final w = c.read(wifiNotifierProvider(_devId)).value!;
    expect(w.state, 'failed');
    expect(w.lastError, 'auth');
  });
}
