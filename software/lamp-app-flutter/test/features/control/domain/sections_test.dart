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

  test('ShadeSection parses single color', () {
    final s = ShadeSection.fromJson(jsonDecode(
      '{"px":38,"bpp":4,"colors":["#000000FF"]}',
    ) as Map<String, dynamic>);
    expect(s.colors.single.w, 0xFF);
  });
}
