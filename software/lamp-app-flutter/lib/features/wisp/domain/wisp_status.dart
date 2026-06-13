import 'dart:convert';
import 'dart:typed_data';

import 'wisp_source_mode.dart';

/// Parsed `CHAR_WISP_STATUS` payload. The lamp serves a merged JSON that
/// combines the last `wispStatus` MSG_CONTROL_OP broadcast (from the
/// wisp) with the last MSG_WISP_HELLO data — see
/// `software/lamp-os/src/components/network/nearby_lamps.cpp` for the
/// merge shape. When no wisp has been seen the lamp returns the empty
/// object `"{}"`; that round-trips into [WispStatus.empty] here.
///
/// Some hello fields may be absent when a status broadcast lands before
/// the first hello (or vice-versa) — every getter handles missing keys
/// gracefully so the UI never has to null-check field by field.
class WispStatus {
  const WispStatus({
    this.currentZone,
    this.zoneSource = 'none',
    this.observedZones = const <int>[],
    this.wifiConnected = false,
    this.auroraConnected = false,
    this.paletteIdPrefix = '',
    this.lastSeenMs,
    this.wispMac,
    this.wispVersion,
    this.helloFlags,
    this.helloPaletteIdPrefix,
    this.helloLastSeenMs,
    this.statusLastSeenMs,
    this.source = WispSourceMode.aurora,
  });

  /// Sentinel for "no wisp has been heard on this lamp yet" (lamp
  /// returned `"{}"`) and the parse-failure fallback.
  static const empty = WispStatus();

  /// Zone the wisp is currently following (i.e. the zone whose palette
  /// it forwards). `null` when [zoneSource] is `"none"`.
  final int? currentZone;

  /// Where [currentZone] came from. Mirrors the wisp's
  /// `ZoneSelector::Source` enum stringified:
  ///   `"none"`        — no zone selected, wisp is idle
  ///   `"firstSeen"`   — wisp adopted the first zone it observed on the mesh
  ///   `"nvs"`         — operator persisted a choice from a previous session
  ///   `"appOp"`       — operator set it this session via a `setZone` wispOp
  final String zoneSource;

  /// Zone IDs the wisp has heard recently on the mesh. Drives the
  /// zone-picker chip list in the UI.
  final List<int> observedZones;

  /// Wisp's WiFi radio link to the home AP.
  final bool wifiConnected;

  /// Wisp's TCP/WS link to Aurora. False whenever WiFi is down; can
  /// also be false while WiFi is up but Aurora is unreachable.
  final bool auroraConnected;

  /// First 8 hex chars of the palette ID the wisp last published.
  /// Empty string when the wisp hasn't published a palette yet.
  final String paletteIdPrefix;

  /// Wisp's local `millis()` at the moment it serialised this status.
  /// NOT comparable across reboots; the UI shows "last seen" relative
  /// to the phone's local clock instead (computed at notify time).
  final int? lastSeenMs;

  /// Wisp's mesh MAC address, uppercase colon-separated (e.g.
  /// `"AA:BB:CC:DD:EE:FF"`). `null` when no wisp has been observed.
  final String? wispMac;

  /// Wisp firmware version (semantic-versioned u32 in hello).
  final int? wispVersion;

  /// Hello capability flags. Bit layout owned by the wisp side.
  final int? helloFlags;

  /// 8-char palette prefix carried in the hello payload (may differ
  /// from [paletteIdPrefix] if a status arrived more recently than
  /// the last hello).
  final String? helloPaletteIdPrefix;

  /// Lamp-side `lastSeenMs` for the hello half of the merge.
  final int? helloLastSeenMs;

  /// Lamp-side `lastSeenMs` for the status half of the merge.
  final int? statusLastSeenMs;

  /// Phase E source-mode: off / manual / aurora. Drives the top-of-pane
  /// pill picker. Defaults to [WispSourceMode.aurora] when missing so
  /// pre-Phase-E wisps and `{}` payloads land on the legacy default.
  final WispSourceMode source;

  /// True iff Aurora has ever been observed on the mesh from this wisp's
  /// perspective. The Aurora pill in the source picker is enabled only
  /// when this is true — selecting Aurora before the wisp has heard a
  /// real zone would just produce an empty palette.
  ///
  /// We treat "Aurora detected" as either:
  ///   • `auroraConnected == true` right now, OR
  ///   • the wisp has logged at least one [observedZones] entry.
  /// Either is sufficient evidence that there's a zone to follow.
  bool get auroraDetected =>
      auroraConnected || observedZones.isNotEmpty || currentZone != null;

  /// True iff the lamp has observed any wisp on the mesh. A wisp is
  /// "present" once either a hello or a status has arrived; the merged
  /// payload always carries a [wispMac] in that case.
  bool get present => wispMac != null;

  /// Decode the raw BLE characteristic bytes. Tolerates an empty
  /// payload, `"{}"`, and malformed JSON — all map to [empty]. Hello
  /// vs status fields are independent; missing keys default to the
  /// safest "we don't know" value.
  factory WispStatus.fromBytes(Uint8List bytes) {
    if (bytes.isEmpty) return empty;
    final dynamic decoded;
    try {
      decoded = jsonDecode(utf8.decode(bytes));
    } catch (_) {
      return empty;
    }
    if (decoded is! Map<String, dynamic>) return empty;
    return WispStatus.fromJson(decoded);
  }

  factory WispStatus.fromJson(Map<String, dynamic> json) {
    int? asInt(Object? v) {
      if (v is int) return v;
      if (v is num) return v.toInt();
      if (v is String) return int.tryParse(v);
      return null;
    }

    bool asBool(Object? v) {
      if (v is bool) return v;
      if (v is num) return v != 0;
      return false;
    }

    String asString(Object? v) {
      if (v is String) return v;
      return '';
    }

    List<int> asIntList(Object? v) {
      if (v is List) {
        return [
          for (final item in v)
            if (asInt(item) != null) asInt(item)!,
        ];
      }
      return const <int>[];
    }

    final zoneSrc = asString(json['zoneSource']);
    final sourceRaw = json['source'];
    return WispStatus(
      currentZone: asInt(json['currentZone']),
      zoneSource: zoneSrc.isEmpty ? 'none' : zoneSrc,
      observedZones: asIntList(json['observedZones']),
      wifiConnected: asBool(json['wifiConnected']),
      auroraConnected: asBool(json['auroraConnected']),
      paletteIdPrefix: asString(json['paletteIdPrefix']),
      lastSeenMs: asInt(json['lastSeenMs']),
      wispMac: json['wispMac'] is String
          ? json['wispMac'] as String
          : null,
      wispVersion: asInt(json['wispVersion']),
      helloFlags: asInt(json['helloFlags']),
      helloPaletteIdPrefix: json['helloPaletteIdPrefix'] is String
          ? json['helloPaletteIdPrefix'] as String
          : null,
      helloLastSeenMs: asInt(json['helloLastSeenMs']),
      statusLastSeenMs: asInt(json['statusLastSeenMs']),
      // parseWispSourceMode tolerates null + unknown strings; the default
      // is aurora, matching the wisp-side coercion.
      source: parseWispSourceMode(sourceRaw is String ? sourceRaw : null),
    );
  }

  WispStatus copyWith({
    int? currentZone,
    String? zoneSource,
    List<int>? observedZones,
    WispSourceMode? source,
  }) {
    return WispStatus(
      currentZone: currentZone ?? this.currentZone,
      zoneSource: zoneSource ?? this.zoneSource,
      observedZones: observedZones ?? this.observedZones,
      wifiConnected: wifiConnected,
      auroraConnected: auroraConnected,
      paletteIdPrefix: paletteIdPrefix,
      lastSeenMs: lastSeenMs,
      wispMac: wispMac,
      wispVersion: wispVersion,
      helloFlags: helloFlags,
      helloPaletteIdPrefix: helloPaletteIdPrefix,
      helloLastSeenMs: helloLastSeenMs,
      statusLastSeenMs: statusLastSeenMs,
      source: source ?? this.source,
    );
  }
}
