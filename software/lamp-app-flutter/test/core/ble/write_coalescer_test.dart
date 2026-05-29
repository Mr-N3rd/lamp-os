import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/write_coalescer.dart';

void main() {
  test('debounces back-to-back schedules into a single write', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(milliseconds: 20),
    );
    c.schedule(Uint8List.fromList([1]));
    c.schedule(Uint8List.fromList([2]));
    c.schedule(Uint8List.fromList([3]));
    await Future<void>.delayed(const Duration(milliseconds: 40));
    expect(writes, [Uint8List.fromList([3])]);
  });

  test('flush() forces immediate send and cancels pending timer', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(seconds: 10),
    );
    c.schedule(Uint8List.fromList([1]));
    await c.flush();
    expect(writes, [Uint8List.fromList([1])]);
  });

  test('dispose() cancels pending work', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(milliseconds: 20),
    );
    c.schedule(Uint8List.fromList([1]));
    c.dispose();
    await Future<void>.delayed(const Duration(milliseconds: 40));
    expect(writes, isEmpty);
  });
}
