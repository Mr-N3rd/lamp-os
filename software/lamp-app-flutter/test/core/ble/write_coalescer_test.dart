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
