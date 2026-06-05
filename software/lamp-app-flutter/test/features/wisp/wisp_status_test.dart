import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/wisp/domain/wisp_status.dart';

/// Wrap a JSON string as the raw UTF-8 bytes the BLE layer hands us.
Uint8List _b(String s) => Uint8List.fromList(utf8.encode(s));

void main() {
  group('WispStatus.fromBytes', () {
    test('empty bytes → empty status (not present)', () {
      final s = WispStatus.fromBytes(Uint8List(0));
      expect(s.present, isFalse);
      expect(s.wispMac, isNull);
      expect(s.currentZone, isNull);
      expect(s.zoneSource, 'none');
      expect(s.observedZones, isEmpty);
    });

    test('"{}" bytes → empty status (not present)', () {
      final s = WispStatus.fromBytes(_b('{}'));
      expect(s.present, isFalse);
      expect(s.wispMac, isNull);
      expect(s.currentZone, isNull);
      expect(s.zoneSource, 'none');
    });

    test('invalid UTF-8 bytes → empty status (no throw)', () {
      // 0xC3 is a UTF-8 lead byte with no continuation — utf8.decode
      // throws on this in strict mode. Parser must catch it.
      final bytes = Uint8List.fromList([0xC3, 0x28]);
      final s = WispStatus.fromBytes(bytes);
      expect(s.present, isFalse);
    });

    test('malformed JSON bytes → empty status (no throw)', () {
      final s = WispStatus.fromBytes(_b('{not json'));
      expect(s.present, isFalse);
    });

    test('non-object JSON root → empty status', () {
      // A bare array or string at the root isn't a valid wisp payload.
      final s = WispStatus.fromBytes(_b('[1,2,3]'));
      expect(s.present, isFalse);
    });

    test('canonical payload parses every field', () {
      final s = WispStatus.fromBytes(_b(
        '{"wispMac":"AA:BB:CC:DD:EE:FF",'
        '"currentZone":3,'
        '"zoneSource":"nvs",'
        '"observedZones":[0,3,7],'
        '"wifiConnected":true,'
        '"auroraConnected":true}',
      ));
      expect(s.present, isTrue);
      expect(s.wispMac, 'AA:BB:CC:DD:EE:FF');
      expect(s.currentZone, 3);
      expect(s.zoneSource, 'nvs');
      expect(s.observedZones, [0, 3, 7]);
      expect(s.wifiConnected, isTrue);
      expect(s.auroraConnected, isTrue);
    });

    test('zoneSource "firstSeen" round-trips', () {
      final s = WispStatus.fromBytes(_b(
        '{"wispMac":"AA:BB:CC:DD:EE:FF","zoneSource":"firstSeen"}',
      ));
      expect(s.zoneSource, 'firstSeen');
    });

    test('zoneSource "appOp" round-trips', () {
      final s = WispStatus.fromBytes(_b(
        '{"wispMac":"AA:BB:CC:DD:EE:FF","zoneSource":"appOp"}',
      ));
      expect(s.zoneSource, 'appOp');
    });

    test('zoneSource "none" round-trips', () {
      final s = WispStatus.fromBytes(_b(
        '{"wispMac":"AA:BB:CC:DD:EE:FF","zoneSource":"none"}',
      ));
      expect(s.zoneSource, 'none');
    });

    test('extra unknown keys do not crash', () {
      // Forward-compat: future wisp firmware may add fields the app
      // doesn't know about. Parser must ignore them gracefully.
      final s = WispStatus.fromBytes(_b(
        '{"wispMac":"AA:BB:CC:DD:EE:FF",'
        '"currentZone":1,'
        '"futureField":"whatever",'
        '"anotherFutureField":42,'
        '"nestedFuture":{"a":1,"b":[2,3]}}',
      ));
      expect(s.present, isTrue);
      expect(s.currentZone, 1);
    });

    test('missing observedZones defaults to empty list', () {
      final s = WispStatus.fromBytes(_b(
        '{"wispMac":"AA:BB:CC:DD:EE:FF","currentZone":1}',
      ));
      expect(s.observedZones, isEmpty);
    });
  });
}
