import 'dart:async';
import 'dart:typed_data';

class BleNotFound implements Exception {
  const BleNotFound(this.message);
  final String message;
  @override
  String toString() => 'BleNotFound: $message';
}

class BleEncryptionRequired implements Exception {
  const BleEncryptionRequired(this.deviceId);
  final String deviceId;
  @override
  String toString() => 'BleEncryptionRequired: $deviceId';
}

class BleNotConnected implements Exception {
  const BleNotConnected(this.deviceId);
  final String deviceId;
  @override
  String toString() => 'BleNotConnected: $deviceId';
}

abstract class BleClient {
  Future<void> connect(String deviceId);
  Future<void> disconnect(String deviceId);
  bool isConnected(String deviceId);
  Future<Uint8List> read(String deviceId, String serviceUuid, String charUuid);
  Future<void> write(
    String deviceId,
    String serviceUuid,
    String charUuid,
    Uint8List value,
  );
  Stream<Uint8List> subscribe(
    String deviceId,
    String serviceUuid,
    String charUuid,
  );

  /// Emits the current connection state immediately on listen, then emits on
  /// every change. Used by callers to react to unsolicited link drops
  /// (e.g. LINK_SUPERVISION_TIMEOUT on Android).
  Stream<bool> watchConnected(String deviceId);
}

/// Test/dev fake. Records writes per (device, service, char) key, lets tests
/// schedule encryption failures, and broadcasts writes to subscribers.
class InMemoryBleClient implements BleClient {
  final Set<String> _connected = {};
  final Map<String, Uint8List> _values = {};
  final Map<String, StreamController<Uint8List>> _streams = {};
  final Map<String, StreamController<bool>> _connStreams = {};
  final Map<String, int> _pendingEncryptionFails = {};

  String _key(String d, String s, String c) => '$d|$s|$c';

  StreamController<bool> _ensureConnStream(String deviceId) {
    return _connStreams.putIfAbsent(
      deviceId,
      () => StreamController<bool>.broadcast(),
    );
  }

  void scheduleEncryptionFailure(String d, String s, String c) {
    final key = _key(d, s, c);
    _pendingEncryptionFails[key] = (_pendingEncryptionFails[key] ?? 0) + 1;
  }

  @override
  Future<void> connect(String deviceId) async {
    _connected.add(deviceId);
    _ensureConnStream(deviceId).add(true);
  }

  @override
  Future<void> disconnect(String deviceId) async {
    _connected.remove(deviceId);
    _ensureConnStream(deviceId).add(false);
  }

  @override
  bool isConnected(String deviceId) => _connected.contains(deviceId);

  @override
  Future<Uint8List> read(String d, String s, String c) async {
    if (!_connected.contains(d)) throw BleNotConnected(d);
    final v = _values[_key(d, s, c)];
    if (v == null) throw BleNotFound('$d/$s/$c');
    return v;
  }

  @override
  Future<void> write(String d, String s, String c, Uint8List v) async {
    if (!_connected.contains(d)) throw BleNotConnected(d);
    final key = _key(d, s, c);
    final count = _pendingEncryptionFails[key] ?? 0;
    if (count > 0) {
      _pendingEncryptionFails[key] = count - 1;
      throw BleEncryptionRequired(d);
    }
    _values[key] = v;
    _streams[key]?.add(v);
  }

  @override
  Stream<Uint8List> subscribe(String d, String s, String c) {
    if (!_connected.contains(d)) throw BleNotConnected(d);
    final key = _key(d, s, c);
    final ctrl = _streams.putIfAbsent(
      key,
      () => StreamController<Uint8List>.broadcast(),
    );
    return ctrl.stream;
  }

  @override
  Stream<bool> watchConnected(String deviceId) {
    final ctrl = _ensureConnStream(deviceId);
    // Subscribe to the broadcast stream BEFORE emitting the initial value so
    // that no events are dropped between the seed yield and yield*. Using a
    // StreamController lets us prepend the current state without the async*
    // generator's subscription-before-yield race condition.
    final out = StreamController<bool>();
    final sub = ctrl.stream.listen(
      out.add,
      onError: out.addError,
      onDone: out.close,
    );
    out.onCancel = sub.cancel;
    // Emit current state immediately so callers don't have to call isConnected
    // separately to seed their state.
    out.add(isConnected(deviceId));
    return out.stream;
  }
}
