import 'dart:typed_data';

import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/write_coalescer.dart';

void main() {
  test('leading edge fires immediately; trailing fires with the last payload',
      () {
    fakeAsync((async) {
      final writes = <Uint8List>[];
      final c = WriteCoalescer(
        onWrite: (v) async => writes.add(v),
        debounce: const Duration(milliseconds: 20),
      );
      // Burst of three schedules inside one window.
      c.schedule(Uint8List.fromList([1]));
      c.schedule(Uint8List.fromList([2]));
      c.schedule(Uint8List.fromList([3]));
      // First schedule lands immediately (leading edge); 2 and 3 are
      // coalesced into the trailing flush which fires at +20ms.
      async.elapse(const Duration(milliseconds: 40));
      expect(writes,
          [Uint8List.fromList([1]), Uint8List.fromList([3])]);
    });
  });

  test('each idle window fires exactly once on the leading edge', () {
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

  test('flush() forces the pending trailing payload to land', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(seconds: 10),
    );
    // First schedule fires the leading edge.
    c.schedule(Uint8List.fromList([1]));
    // Second schedule is coalesced as the pending trailing payload.
    c.schedule(Uint8List.fromList([2]));
    await c.flush();
    expect(writes, [Uint8List.fromList([1]), Uint8List.fromList([2])]);
  });

  test('flush() after dispose() is a no-op', () async {
    final writes = <Uint8List>[];
    final c = WriteCoalescer(
      onWrite: (v) async => writes.add(v),
      debounce: const Duration(seconds: 10),
    );
    // First schedule fires leading-edge.
    c.schedule(Uint8List.fromList([1]));
    // Second schedule queues a trailing payload that dispose() should drop.
    c.schedule(Uint8List.fromList([2]));
    c.dispose();
    await c.flush();
    // Only the leading-edge write made it; the trailing payload was
    // cancelled by dispose() and flush() is a no-op after dispose.
    expect(writes, [Uint8List.fromList([1])]);
  });

  test('dispose() cancels the trailing flush', () {
    fakeAsync((async) {
      final writes = <Uint8List>[];
      final c = WriteCoalescer(
        onWrite: (v) async => writes.add(v),
        debounce: const Duration(milliseconds: 20),
      );
      c.schedule(Uint8List.fromList([1])); // leading-edge fires
      c.schedule(Uint8List.fromList([2])); // queued as trailing
      c.dispose();
      async.elapse(const Duration(milliseconds: 40));
      expect(writes, [Uint8List.fromList([1])]);
    });
  });
}
