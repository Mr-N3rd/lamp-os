import 'dart:async';

typedef FlushFn<T> = Future<void> Function(T value);

class WriteCoalescer<T> {
  WriteCoalescer({required this.debounce, required this.onFlush});

  final Duration debounce;
  final FlushFn<T> onFlush;
  Timer? _timer;
  T? _pending;
  bool _hasPending = false;

  void schedule(T value) {
    _pending = value;
    _hasPending = true;
    _timer?.cancel();
    _timer = Timer(debounce, _fire);
  }

  Future<void> flushNow() async {
    _timer?.cancel();
    _timer = null;
    await _fire();
  }

  Future<void> _fire() async {
    if (!_hasPending) return;
    final v = _pending as T;
    _hasPending = false;
    _pending = null;
    await onFlush(v);
  }

  void dispose() => _timer?.cancel();
}
