import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:lamp_app/core/ble/write_coalescer.dart';

void main() {
  test('rapid writes coalesce to the latest payload', () {
    fakeAsync((async) {
      final flushed = <int>[];
      final c = WriteCoalescer<int>(
        debounce: const Duration(milliseconds: 30),
        onFlush: (v) async => flushed.add(v),
      );

      c.schedule(1);
      async.elapse(const Duration(milliseconds: 10));
      c.schedule(2);
      async.elapse(const Duration(milliseconds: 10));
      c.schedule(3);
      async.elapse(const Duration(milliseconds: 40));

      expect(flushed, [3]);
    });
  });

  test('isolated writes each flush', () {
    fakeAsync((async) {
      final flushed = <int>[];
      final c = WriteCoalescer<int>(
        debounce: const Duration(milliseconds: 30),
        onFlush: (v) async => flushed.add(v),
      );

      c.schedule(1);
      async.elapse(const Duration(milliseconds: 100));
      c.schedule(2);
      async.elapse(const Duration(milliseconds: 100));

      expect(flushed, [1, 2]);
    });
  });

  test('flushNow bypasses debounce', () async {
    final flushed = <int>[];
    final c = WriteCoalescer<int>(
      debounce: const Duration(milliseconds: 30),
      onFlush: (v) async => flushed.add(v),
    );
    c.schedule(7);
    await c.flushNow();
    expect(flushed, [7]);
  });
}
