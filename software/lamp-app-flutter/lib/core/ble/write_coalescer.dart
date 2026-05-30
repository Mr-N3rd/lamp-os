import 'dart:async';
import 'dart:typed_data';

/// Throttles a stream of [schedule] calls so the BLE link sees at most one
/// write per [debounce] window plus a trailing write with the final value.
/// Used to keep slider drags from saturating the GATT queue while still
/// letting the lamp track the slider in real time.
///
/// Behavior:
///   1. The first `schedule` after an idle window fires immediately.
///   2. Subsequent calls within the window stash the payload as `_pending`.
///   3. When the window elapses the pending payload is drained.
///   4. Calls after the trailing drain start a fresh window at #1.
///
/// This replaces the previous pure trailing-edge debouncer, which under
/// continuous slider drag never reached the timer-fire point — every new
/// call cancelled the timer — so zero writes went out until the user
/// stopped moving. With throttle semantics the lamp now sees a steady
/// ~`1000ms/debounce` writes during the drag and the final value lands
/// within one window after the user lets go.
///
/// Errors from [onWrite] are intentionally dropped — this is a
/// fire-and-forget channel; a failed mid-stream slider write should not
/// crash the UI.
class WriteCoalescer {
  WriteCoalescer({required this.onWrite, required this.debounce});

  final Future<void> Function(Uint8List) onWrite;

  /// Minimum time between writes. Also bounds the latency between the last
  /// `schedule` and the trailing flush.
  final Duration debounce;

  Uint8List? _pending;
  Timer? _windowTimer;
  bool _windowOpen = false;
  bool _disposed = false;

  void schedule(Uint8List payload) {
    if (_disposed) return;
    if (!_windowOpen) {
      // Leading edge — send immediately and open the throttle window.
      _windowOpen = true;
      _pending = null;
      unawaited(onWrite(payload));
      _windowTimer = Timer(debounce, _onWindowElapsed);
      return;
    }
    // Inside the window — coalesce; only the latest payload survives.
    _pending = payload;
  }

  Future<void> flush() async {
    if (_disposed) return;
    _windowTimer?.cancel();
    final payload = _pending;
    _pending = null;
    _windowOpen = false;
    if (payload != null) {
      await onWrite(payload);
    }
  }

  void _onWindowElapsed() {
    if (_disposed) return;
    final payload = _pending;
    _pending = null;
    if (payload != null) {
      // Trailing edge — send the final value and keep the window open for
      // another debounce so a still-active drag continues to throttle.
      unawaited(onWrite(payload));
      _windowTimer = Timer(debounce, _onWindowElapsed);
    } else {
      // No new calls during the window — close it; the next call will
      // re-enter via the leading edge.
      _windowOpen = false;
    }
  }

  void dispose() {
    _disposed = true;
    _windowTimer?.cancel();
    _windowTimer = null;
    _pending = null;
    _windowOpen = false;
  }
}
