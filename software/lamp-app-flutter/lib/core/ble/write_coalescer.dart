import 'dart:async';
import 'dart:typed_data';

/// Coalesces a stream of [schedule] calls into a single trailing write after
/// [debounce] has elapsed since the last call. Used to keep slider drags from
/// firing dozens of BLE writes per second.
///
/// Errors from [onWrite] are intentionally dropped — this is a fire-and-forget
/// debouncer; a failed mid-stream slider write should not crash the UI.
class WriteCoalescer {
  WriteCoalescer({required this.onWrite, required this.debounce});

  final Future<void> Function(Uint8List) onWrite;
  final Duration debounce;

  Uint8List? _pending;
  Timer? _timer;
  bool _disposed = false;

  void schedule(Uint8List payload) {
    if (_disposed) return;
    _pending = payload;
    _timer?.cancel();
    _timer = Timer(debounce, () => unawaited(_drain()));
  }

  Future<void> flush() async {
    if (_disposed) return;
    _timer?.cancel();
    await _drain();
  }

  Future<void> _drain() async {
    final payload = _pending;
    _pending = null;
    if (payload != null) {
      await onWrite(payload);
    }
  }

  void dispose() {
    _disposed = true;
    _timer?.cancel();
    _pending = null;
  }
}
