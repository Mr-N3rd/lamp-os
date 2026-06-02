/// One historical sighting of a lamp via BLE adv. Persisted in
/// SharedPreferences so the Info page can show lamps you've met before
/// even when they're not currently broadcasting. Plain class (no Freezed)
/// — the fields are stable and the JSON shape is simple; build_runner
/// would be overhead for no gain.
class SeenLamp {
  const SeenLamp({
    required this.id,
    required this.name,
    required this.baseRgb,
    required this.shadeRgb,
    required this.lastSeenEpochMs,
  });

  /// BLE MAC / remoteId — the same key used in NearbyLamp and InventoryLamp.
  final String id;

  /// Whatever name the lamp was broadcasting last time we heard from it.
  final String name;

  /// Last seen base colour, packed as 0xRRGGBB.
  final int baseRgb;

  /// Last seen shade colour. Defaults to 0 when the lamp's adv didn't
  /// include shade (legacy 6-byte payloads).
  final int shadeRgb;

  final int lastSeenEpochMs;

  Map<String, dynamic> toJson() => {
        'id': id,
        'name': name,
        'baseRgb': baseRgb,
        'shadeRgb': shadeRgb,
        'lastSeenEpochMs': lastSeenEpochMs,
      };

  factory SeenLamp.fromJson(Map<String, dynamic> json) => SeenLamp(
        id: json['id'] as String,
        name: (json['name'] as String?) ?? '',
        baseRgb: (json['baseRgb'] as num?)?.toInt() ?? 0,
        shadeRgb: (json['shadeRgb'] as num?)?.toInt() ?? 0,
        lastSeenEpochMs: (json['lastSeenEpochMs'] as num?)?.toInt() ?? 0,
      );

  SeenLamp copyWith({
    String? name,
    int? baseRgb,
    int? shadeRgb,
    int? lastSeenEpochMs,
  }) =>
      SeenLamp(
        id: id,
        name: name ?? this.name,
        baseRgb: baseRgb ?? this.baseRgb,
        shadeRgb: shadeRgb ?? this.shadeRgb,
        lastSeenEpochMs: lastSeenEpochMs ?? this.lastSeenEpochMs,
      );
}
