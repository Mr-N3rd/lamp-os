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

abstract class BleClient {
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
}

/// Test/dev fake. Records writes per (device, service, char) key, lets tests
/// schedule encryption failures, and broadcasts writes to subscribers.
class InMemoryBleClient implements BleClient {
  final Map<String, Uint8List> _values = {};
  final Map<String, StreamController<Uint8List>> _streams = {};
  final Set<String> _pendingEncryptionFails = {};

  String _key(String d, String s, String c) => '$d|$s|$c';

  void scheduleEncryptionFailure(String d, String s, String c) {
    _pendingEncryptionFails.add(_key(d, s, c));
  }

  @override
  Future<Uint8List> read(String d, String s, String c) async {
    final v = _values[_key(d, s, c)];
    if (v == null) throw BleNotFound('$d/$s/$c');
    return v;
  }

  @override
  Future<void> write(String d, String s, String c, Uint8List v) async {
    final key = _key(d, s, c);
    if (_pendingEncryptionFails.remove(key)) {
      throw BleEncryptionRequired(d);
    }
    _values[key] = v;
    _streams[key]?.add(v);
  }

  @override
  Stream<Uint8List> subscribe(String d, String s, String c) {
    final key = _key(d, s, c);
    final ctrl = _streams.putIfAbsent(
      key,
      () => StreamController<Uint8List>.broadcast(),
    );
    return ctrl.stream;
  }
}
