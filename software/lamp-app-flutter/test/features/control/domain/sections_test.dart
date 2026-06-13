import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/features/control/domain/lamp_color.dart';
import 'package:lamp_app/features/control/domain/sections.dart';

void main() {
  test('LampSection parses brightness + name', () {
    final s = LampSection.fromJson(jsonDecode(
      '{"name":"jacko","brightness":42,"advancedEnabled":false}',
    ) as Map<String, dynamic>);
    expect(s.name, 'jacko');
    expect(s.brightness, 42);
  });

  test('LampSection parses fwVersion + fwChannel when emitted', () {
    final s = LampSection.fromJson(jsonDecode(
      '{"name":"jacko","brightness":42,"advancedEnabled":false,'
      '"fwVersion":65536,"fwChannel":"stable"}',
    ) as Map<String, dynamic>);
    expect(s.fwVersion, 0x010000);
    expect(s.fwChannel, 'stable');
  });

  test('LampSection.fwVersion + fwChannel null on legacy firmware', () {
    // Old firmware that doesn't yet emit fwVersion/fwChannel — the Info
    // tab renders these as "..." rather than crashing on a null cast.
    final s = LampSection.fromJson(jsonDecode(
      '{"name":"jacko","brightness":42,"advancedEnabled":false}',
    ) as Map<String, dynamic>);
    expect(s.fwVersion, isNull);
    expect(s.fwChannel, isNull);
  });

  test('BaseSection parses colors, ac, px', () {
    final s = BaseSection.fromJson(jsonDecode(
      '{"px":35,"ac":1,"bpp":4,"colors":["#300783FF","#FF0000AA"],"knockout":[]}',
    ) as Map<String, dynamic>);
    expect(s.px, 35);
    expect(s.ac, 1);
    expect(s.colors.length, 2);
    expect(s.colors[0], const LampColor(r: 0x30, g: 0x07, b: 0x83, w: 0xFF));
    expect(s.colors[1].w, 0xAA);
  });

  test('BaseSection.knockout is empty when the JSON omits it', () {
    final s = BaseSection.fromJson(jsonDecode(
      '{"px":35,"ac":0,"bpp":4,"colors":[]}',
    ) as Map<String, dynamic>);
    expect(s.knockout, isEmpty);
  });

  test('BaseSection.knockout folds entries into a map', () {
    // Positional knockout: index = pixel, value = brightness %. Default
    // (100) entries are skipped on parse, so only non-defaults end up in
    // the map.
    final s = BaseSection.fromJson(jsonDecode(
      '{"px":35,"ac":0,"bpp":4,"colors":[],"knockout":[100,100,100,50,100,100,100,25,100,100]}',
    ) as Map<String, dynamic>);
    expect(s.knockout, {3: 50, 7: 25});
  });

  test('ShadeSection parses single color', () {
    final s = ShadeSection.fromJson(jsonDecode(
      '{"px":38,"bpp":4,"colors":["#000000FF"]}',
    ) as Map<String, dynamic>);
    expect(s.colors.single.w, 0xFF);
  });

  test('HomeSection parses ssid + brightness (legacy password field ignored)',
      () {
    // Legacy lamps wrote a "password" field — we silently ignore it now.
    final s = HomeSection.fromJson(jsonDecode(
      '{"ssid":"home","password":"********","brightness":40}',
    ) as Map<String, dynamic>);
    expect(s.ssid, 'home');
    expect(s.brightness, 40);
  });

  test('HomeSection defaults on a sparse JSON', () {
    final s = HomeSection.fromJson(<String, dynamic>{});
    expect(s.ssid, '');
    expect(s.brightness, 60);
  });

  test('ExpressionConfig round-trips through toJson + fromJson', () {
    final original = ExpressionConfig(
      type: 'glitchy',
      enabled: true,
      colors: [LampColor.fromHex('#FF00FFAA')],
      intervalMin: 30,
      intervalMax: 120,
      target: 2,
      parameters: {'flickerRate': 5, 'jitter': 100},
    );
    final round = ExpressionConfig.fromJson(
      Map<String, dynamic>.from(
          jsonDecode(jsonEncode(original.toJson())) as Map),
    );
    expect(round.type, 'glitchy');
    expect(round.enabled, isTrue);
    expect(round.colors.single.toHex(), '#FF00FFAA');
    expect(round.intervalMin, 30);
    expect(round.intervalMax, 120);
    expect(round.target, 2);
    expect(round.parameters, {'flickerRate': 5, 'jitter': 100});
  });

  test('ExpressionsSection parses an empty array', () {
    expect(ExpressionsSection.fromJson([]).expressions, isEmpty);
  });

  test('ExpressionsSection parses two entries', () {
    final s = ExpressionsSection.fromJson(
      (jsonDecode(
        '[{"type":"breathing","enabled":true,"colors":[],"intervalMin":10,"intervalMax":20,"target":1},'
        '{"type":"glitchy","enabled":false,"colors":[],"intervalMin":60,"intervalMax":900,"target":3}]',
      ) as List).cast<Map<String, dynamic>>(),
    );
    expect(s.expressions, hasLength(2));
    expect(s.expressions[0].type, 'breathing');
    expect(s.expressions[1].target, 3);
  });
}
