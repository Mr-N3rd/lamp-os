import 'dart:typed_data';

import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/write_coalescer.dart';

void main() {
  test('debounces back-to-back schedules into a single write', () {
    fakeAsync((async) {
      final writes = <Uint8List>[];
      final c = WriteCoalescer(
        onWrite: (v) async => writes.add(v),
        debounce: const Duration(milliseconds: 20),
      );
      c.schedule(Uint8List.fromList([1]));
      c.schedule(Uint8List.fromList([2]));
      c.schedule(Uint8List.fromList([3]));
      async.elapse(const Duration(milliseconds: 40));
      expect(writes, [Uint8List.fromList([3])]);
    });
  });

  test('each debounce window fires exactly once', () {
    fakeAsync((async) {
      final writes = <Uint8List>[];
      final c = WriteCoalescer(
        onWrite: (v) async => writes.add(v),
        debounce: const Duration(milliseconds: 20),
      );
      c.schedule(Uint8List.fromList([1]));
      async.elapse(const Duration(milliseconds: 30));
      c.schedule(Uint8List.fromList([2]));
      async.elapse(const Duration(milliseconds: 30));
      expect(writes, [Uint8List.fromList([1]), Uint8List.fromList([2])]);
    });
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

  test('flush() after dispose() is a no-op', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(seconds: 10),
    );
    c.schedule(Uint8List.fromList([1]));
    c.dispose();
    await c.flush();
    expect(writes, isEmpty);
  });

  test('dispose() cancels pending work', () {
    fakeAsync((async) {
      final writes = <Uint8List>[];
      final c = WriteCoalescer(
        onWrite: (v) async => writes.add(v),
        debounce: const Duration(milliseconds: 20),
      );
      c.schedule(Uint8List.fromList([1]));
      c.dispose();
      async.elapse(const Duration(milliseconds: 40));
      expect(writes, isEmpty);
    });
  });
}
