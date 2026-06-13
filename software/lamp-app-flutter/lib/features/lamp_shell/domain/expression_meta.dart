import 'package:flutter/material.dart';

/// Descriptive metadata for the four expression types the firmware ships.
/// Pulled from the C++ headers under `software/lamp-os/src/expressions/`.
class ExpressionTypeMeta {
  const ExpressionTypeMeta({
    required this.key,
    required this.name,
    required this.icon,
    required this.tagline,
    required this.defaultParameters,
  });

  /// Wire-level identifier the firmware switches on (`expression.type`).
  final String key;

  /// Human-friendly label shown in the picker and editor header.
  final String name;

  /// Single-character glyph for the picker tile.
  final IconData icon;

  /// One-line description for the picker card. Keep under ~80 chars.
  final String tagline;

  /// Parameter defaults that match the firmware's hard-coded defaults so a
  /// freshly-created expression behaves the same as a no-parameters version.
  /// All values are `uint32_t` on the firmware side.
  final Map<String, int> defaultParameters;

  /// Ordered for picker display.
  static const all = <ExpressionTypeMeta>[
    ExpressionTypeMeta(
      key: 'breathing',
      name: 'Breathing',
      icon: Icons.air,
      tagline: 'A gentle, continuous breath between palette colors.',
      defaultParameters: {'breathSpeed': 10},
    ),
    ExpressionTypeMeta(
      key: 'pulse',
      name: 'Pulse',
      icon: Icons.graphic_eq,
      tagline: 'A wave of color that sweeps across the strip.',
      defaultParameters: {'pulseSpeed': 3},
    ),
    ExpressionTypeMeta(
      key: 'shifty',
      name: 'Shifty',
      icon: Icons.shuffle,
      tagline: 'Slow ambient drift toward random palette colors.',
      defaultParameters: {
        'shiftDurationMin': 300,
        'shiftDurationMax': 600,
        'fadeDuration': 60,
      },
    ),
    ExpressionTypeMeta(
      key: 'glitchy',
      name: 'Glitchy',
      icon: Icons.bolt,
      tagline: 'Rare, sudden flickers of a random palette color.',
      defaultParameters: {'durationMin': 1, 'durationMax': 3},
    ),
  ];

  static ExpressionTypeMeta? byKey(String key) {
    for (final m in all) {
      if (m.key == key) return m;
    }
    return null;
  }
}

/// User-facing label for the three [ExpressionTarget] values.
String targetLabel(int target) => switch (target) {
      1 => 'Shade',
      2 => 'Base',
      3 => 'Both',
      _ => 'Both',
    };
