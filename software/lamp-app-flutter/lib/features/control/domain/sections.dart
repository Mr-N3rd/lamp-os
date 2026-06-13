import 'package:collection/collection.dart';

import '../../social/domain/social_mode.dart';
import 'lamp_color.dart';

const _listEq = ListEquality<Object?>();
const _mapEq = MapEquality<Object?, Object?>();

/// CHAR_LAMP_SECTION payload, see firmware Config::asLampJson.
///
/// Manually-overridden `==` / `hashCode` (audit cq-H / W7.7): Riverpod's
/// `.select` and AsyncValue equality short-circuit on `prev == next`.
/// Without explicit equality the default identity compare always misses
/// — every notifier rebuild propagated to every consumer even when no
/// observable field changed. Full @freezed conversion was rejected
/// because the `fromJson` factories carry non-trivial coercion logic
/// (knockout list → map, byteOrder fallback from bpp, etc.) that the
/// generator can't faithfully reproduce.
class LampSection {
  const LampSection({
    required this.name,
    required this.brightness,
    required this.advancedEnabled,
    required this.socialMode,
    this.fwVersion,
    this.fwChannel,
  });

  final String name;
  final int brightness;
  final bool advancedEnabled;
  final SocialMode socialMode;

  /// Firmware semver, packed as `(major << 16) | (minor << 8) | patch`.
  /// Nullable for backward compat with older firmware that doesn't yet
  /// emit this field (the Info tab renders "..." in that case).
  final int? fwVersion;

  /// Firmware release channel: `'dev' | 'beta' | 'stable'`. Nullable for
  /// the same backward-compat reason as `fwVersion`.
  final String? fwChannel;

  factory LampSection.fromJson(Map<String, dynamic> json) => LampSection(
        name: (json['name'] as String?) ?? '',
        brightness: (json['brightness'] as num?)?.toInt() ?? 100,
        advancedEnabled: json['advancedEnabled'] as bool? ?? false,
        socialMode:
            SocialMode.fromWire((json['socialMode'] as num?)?.toInt()),
        fwVersion: (json['fwVersion'] as num?)?.toInt(),
        fwChannel: json['fwChannel'] as String?,
      );

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is LampSection &&
          name == other.name &&
          brightness == other.brightness &&
          advancedEnabled == other.advancedEnabled &&
          socialMode == other.socialMode &&
          fwVersion == other.fwVersion &&
          fwChannel == other.fwChannel;

  @override
  int get hashCode => Object.hash(
        name,
        brightness,
        advancedEnabled,
        socialMode,
        fwVersion,
        fwChannel,
      );
}

/// CHAR_BASE_SECTION payload, see firmware Config::asBaseJson.
class BaseSection {
  const BaseSection({
    required this.px,
    required this.ac,
    required this.bpp,
    required this.byteOrder,
    required this.colors,
    required this.knockout,
  });

  final int px;
  final int ac;
  final int bpp;

  /// NeoPixel wire byte order: `GRBW` (4 bpp), `GRB` (3 bpp), or `BGR`
  /// (3 bpp). Source of truth for strip type; `bpp` is kept in sync so
  /// older firmware that hasn't grown the `byteOrder` field still
  /// behaves correctly via the bpp-derived NeoPixel default.
  final String byteOrder;

  final List<LampColor> colors;

  /// Per-LED brightness overrides (0..100). Indices absent from the map use
  /// the default 100 %. Empty map = all LEDs at full brightness. Stored as a
  /// `Map<int, int>` rather than a list for O(1) lookup and small memory.
  final Map<int, int> knockout;

  factory BaseSection.fromJson(Map<String, dynamic> json) {
    // Knockout is a positional int array: index = pixel, value = brightness
    // % (0..100, default 100). Only non-default entries are kept in the
    // map (firmware's `asBaseJson` emits a full-length array, but the app
    // only cares about overrides).
    final knockoutList = (json['knockout'] as List?) ?? const [];
    final knockoutMap = <int, int>{};
    for (var i = 0; i < knockoutList.length; i++) {
      final raw = knockoutList[i];
      if (raw is num) {
        final b = raw.toInt();
        if (b != 100) knockoutMap[i] = b;
      }
    }
    final bpp = (json['bpp'] as num?)?.toInt() ?? 4;
    // `byteOrder` lands as a string on the wire when the firmware
    // supports it. When absent (older firmware), derive from `bpp`.
    final byteOrder = (json['byteOrder'] as String?)?.trim().isNotEmpty == true
        ? json['byteOrder'] as String
        : (bpp == 4 ? 'GRBW' : 'GRB');
    return BaseSection(
      px: (json['px'] as num?)?.toInt() ?? 35,
      ac: (json['ac'] as num?)?.toInt() ?? 0,
      bpp: bpp,
      byteOrder: byteOrder,
      colors: ((json['colors'] as List?) ?? const [])
          .map((e) => LampColor.fromHex(e as String))
          .toList(),
      knockout: knockoutMap,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is BaseSection &&
          px == other.px &&
          ac == other.ac &&
          bpp == other.bpp &&
          byteOrder == other.byteOrder &&
          _listEq.equals(colors, other.colors) &&
          _mapEq.equals(knockout, other.knockout);

  @override
  int get hashCode => Object.hash(
        px,
        ac,
        bpp,
        byteOrder,
        _listEq.hash(colors),
        _mapEq.hash(knockout),
      );
}

/// CHAR_SHADE_SECTION payload, see firmware Config::asShadeJson.
class ShadeSection {
  const ShadeSection({
    required this.px,
    required this.bpp,
    required this.byteOrder,
    required this.colors,
  });

  final int px;
  final int bpp;

  /// NeoPixel wire byte order; see BaseSection.byteOrder.
  final String byteOrder;

  final List<LampColor> colors;

  factory ShadeSection.fromJson(Map<String, dynamic> json) {
    final bpp = (json['bpp'] as num?)?.toInt() ?? 4;
    final byteOrder = (json['byteOrder'] as String?)?.trim().isNotEmpty == true
        ? json['byteOrder'] as String
        : (bpp == 4 ? 'GRBW' : 'GRB');
    return ShadeSection(
      px: (json['px'] as num?)?.toInt() ?? 38,
      bpp: bpp,
      byteOrder: byteOrder,
      colors: ((json['colors'] as List?) ?? const [])
          .map((e) => LampColor.fromHex(e as String))
          .toList(),
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is ShadeSection &&
          px == other.px &&
          bpp == other.bpp &&
          byteOrder == other.byteOrder &&
          _listEq.equals(colors, other.colors);

  @override
  int get hashCode =>
      Object.hash(px, bpp, byteOrder, _listEq.hash(colors));
}

/// CHAR_HOME_SECTION payload. Presence-only home mode: the lamp never
/// stores a password (no association — just SSID-visibility detection),
/// so this section carries only the SSID, brightness, and the soft
/// on/off toggle.
class HomeSection {
  const HomeSection({
    required this.ssid,
    required this.brightness,
    required this.enabled,
  });

  final String ssid;
  final int brightness;

  /// Soft on/off for Home Mode. When false, the lamp ignores SSID
  /// visibility and stays in regular mode.
  final bool enabled;

  factory HomeSection.fromJson(Map<String, dynamic> json) {
    final ssid = (json['ssid'] as String?) ?? '';
    return HomeSection(
      ssid: ssid,
      brightness: (json['brightness'] as num?)?.toInt() ?? 60,
      enabled: (json['enabled'] as bool?) ?? ssid.isNotEmpty,
    );
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is HomeSection &&
          ssid == other.ssid &&
          brightness == other.brightness &&
          enabled == other.enabled;

  @override
  int get hashCode => Object.hash(ssid, brightness, enabled);
}

/// A single expression configuration. CHAR_EXPRESSION_SECTION returns an array
/// of these; see firmware ExpressionConfig.
class ExpressionConfig {
  const ExpressionConfig({
    required this.type,
    required this.enabled,
    required this.colors,
    required this.intervalMin,
    required this.intervalMax,
    required this.target,
    required this.parameters,
    this.disabledDuringWispOverride = false,
  });

  final String type;
  final bool enabled;
  final List<LampColor> colors;
  final int intervalMin;
  final int intervalMax;
  final int target; // 1=shade, 2=base, 3=both
  final Map<String, int> parameters;

  /// When true the lamp skips this expression's auto-trigger while the
  /// wisp is actively overriding either surface (`baseColorOverride` or
  /// `shadeColorOverride` non-Idle with `activeSource == Wisp`).
  /// Defaults to true for `breathing` and `shifty` (they paint
  /// continuously and visibly fight the wisp's hold colour) and false
  /// for `glitchy` / `pulse` (brief flashes / waves that coexist with
  /// a held wisp colour). Operator can flip per-expression in the
  /// editor. See `docs/expressions.md`.
  final bool disabledDuringWispOverride;

  static const _reservedKeys = {
    'type', 'enabled', 'colors', 'intervalMin', 'intervalMax', 'target',
    'disabledDuringWispOverride',
  };

  /// Type-aware value for `disabledDuringWispOverride`. The toggle was
  /// removed from the operator UX in commit 3575f43 — the behavior
  /// should be uniform across the fleet per type. We normalize on load
  /// here so any stale stored value (from when the toggle was
  /// user-toggleable) is overwritten on the next Save with the current
  /// type default. Mirrors firmware-side
  /// `software/lamp-os/src/config/config.cpp`. A future custom-lamps
  /// power-user surface can re-introduce per-instance overrides via a
  /// separate mechanism if needed.
  static bool _defaultDisabledDuringWispOverrideForType(String type) {
    return type == 'breathing' || type == 'shifty';
  }

  factory ExpressionConfig.fromJson(Map<String, dynamic> json) {
    final params = <String, int>{};
    for (final e in json.entries) {
      if (_reservedKeys.contains(e.key)) continue;
      if (e.value is num) params[e.key] = (e.value as num).toInt();
    }
    final type = json['type'] as String? ?? '';
    return ExpressionConfig(
      type: type,
      enabled: json['enabled'] as bool? ?? false,
      colors: ((json['colors'] as List?) ?? const [])
          .map((e) => LampColor.fromHex(e as String))
          .toList(),
      intervalMin: (json['intervalMin'] as num?)?.toInt() ?? 60,
      intervalMax: (json['intervalMax'] as num?)?.toInt() ?? 900,
      target: (json['target'] as num?)?.toInt() ?? 3,
      parameters: params,
      // Force the type default — ignore any stored value. See doc on
      // `_defaultDisabledDuringWispOverrideForType` above for the why.
      disabledDuringWispOverride:
          _defaultDisabledDuringWispOverrideForType(type),
    );
  }

  Map<String, dynamic> toJson() => {
        'type': type,
        'enabled': enabled,
        'colors': colors.map((c) => c.toHex()).toList(),
        'intervalMin': intervalMin,
        'intervalMax': intervalMax,
        'target': target,
        'disabledDuringWispOverride': disabledDuringWispOverride,
        ...parameters,
      };

  ExpressionConfig copyWith({
    String? type,
    bool? enabled,
    List<LampColor>? colors,
    int? intervalMin,
    int? intervalMax,
    int? target,
    Map<String, int>? parameters,
    bool? disabledDuringWispOverride,
  }) =>
      ExpressionConfig(
        type: type ?? this.type,
        enabled: enabled ?? this.enabled,
        colors: colors ?? this.colors,
        intervalMin: intervalMin ?? this.intervalMin,
        intervalMax: intervalMax ?? this.intervalMax,
        target: target ?? this.target,
        parameters: parameters ?? this.parameters,
        disabledDuringWispOverride:
            disabledDuringWispOverride ?? this.disabledDuringWispOverride,
      );

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is ExpressionConfig &&
          type == other.type &&
          enabled == other.enabled &&
          _listEq.equals(colors, other.colors) &&
          intervalMin == other.intervalMin &&
          intervalMax == other.intervalMax &&
          target == other.target &&
          _mapEq.equals(parameters, other.parameters) &&
          disabledDuringWispOverride == other.disabledDuringWispOverride;

  @override
  int get hashCode => Object.hash(
        type,
        enabled,
        _listEq.hash(colors),
        intervalMin,
        intervalMax,
        target,
        _mapEq.hash(parameters),
        disabledDuringWispOverride,
      );
}

/// CHAR_EXPRESSION_SECTION payload — a JSON array of ExpressionConfig objects.
class ExpressionsSection {
  const ExpressionsSection({required this.expressions});

  final List<ExpressionConfig> expressions;

  factory ExpressionsSection.fromJson(List<dynamic> json) =>
      ExpressionsSection(
        expressions: json
            .cast<Map<String, dynamic>>()
            .map(ExpressionConfig.fromJson)
            .toList(),
      );

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is ExpressionsSection &&
          _listEq.equals(expressions, other.expressions);

  @override
  int get hashCode => _listEq.hash(expressions);
}
