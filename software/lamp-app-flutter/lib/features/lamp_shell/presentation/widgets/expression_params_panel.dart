import 'package:flutter/material.dart';

import '../../../../core/theme/brand_colors.dart';

/// Renders the per-type parameter UI for an expression. Replaces the
/// previous raw JSON text field — each parameter the firmware accepts now
/// has a labelled slider with the right units and clamp range.
///
/// Parameter keys + valid ranges are pulled from the firmware
/// (`software/lamp-os/src/expressions/*_expression.cpp`):
///   - breathing.breathSpeed      → 1..60 seconds
///   - pulse.pulseSpeed           → 1..10 seconds (total wave travel time)
///   - shifty.shiftDurationMin/Max → 60..1800 seconds (1..30 min)
///   - shifty.fadeDuration         → 10..300 seconds
///   - glitchy.durationMin/Max    → 1..60 frames (≈ 1/30 s each)
class ExpressionParamsPanel extends StatelessWidget {
  const ExpressionParamsPanel({
    super.key,
    required this.type,
    required this.parameters,
    required this.onChanged,
  });

  final String type;

  /// Current parameter map — keyed by the firmware's parameter name.
  final Map<String, int> parameters;

  /// Called with a new map (NOT a mutated copy of the existing map) so the
  /// parent can drive a notifier update.
  final ValueChanged<Map<String, int>> onChanged;

  int _get(String key, int fallback) => parameters[key] ?? fallback;

  void _set(String key, int value) {
    final next = Map<String, int>.from(parameters);
    next[key] = value;
    onChanged(next);
  }

  void _setBoth(String minKey, int minV, String maxKey, int maxV) {
    final next = Map<String, int>.from(parameters);
    next[minKey] = minV;
    next[maxKey] = maxV;
    onChanged(next);
  }

  @override
  Widget build(BuildContext context) {
    return switch (type) {
      'breathing' => _BreathingParams(
          breathSpeed: _get('breathSpeed', 10),
          onBreathSpeed: (v) => _set('breathSpeed', v),
        ),
      'pulse' => _PulseParams(
          pulseSpeed: _get('pulseSpeed', 3),
          onPulseSpeed: (v) => _set('pulseSpeed', v),
        ),
      'shifty' => _ShiftyParams(
          shiftMin: _get('shiftDurationMin', 300),
          shiftMax: _get('shiftDurationMax', 600),
          fade: _get('fadeDuration', 60),
          onShiftRange: (lo, hi) =>
              _setBoth('shiftDurationMin', lo, 'shiftDurationMax', hi),
          onFade: (v) => _set('fadeDuration', v),
        ),
      'glitchy' => _GlitchyParams(
          durMin: _get('durationMin', 1),
          durMax: _get('durationMax', 3),
          onRange: (lo, hi) =>
              _setBoth('durationMin', lo, 'durationMax', hi),
        ),
      _ => const SizedBox.shrink(),
    };
  }
}

class _SectionLabel extends StatelessWidget {
  const _SectionLabel(this.text);
  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 4, top: 8),
      child: Text(
        text,
        style: const TextStyle(color: BrandColors.lampWhite, fontSize: 14),
      ),
    );
  }
}

class _ValueChip extends StatelessWidget {
  const _ValueChip(this.text);
  final String text;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 84,
      child: Text(
        text,
        textAlign: TextAlign.right,
        style: const TextStyle(
          color: BrandColors.fogGrey,
          fontSize: 12,
          fontFamily: 'monospace',
        ),
      ),
    );
  }
}

class _ParamSlider extends StatelessWidget {
  const _ParamSlider({
    required this.label,
    required this.value,
    required this.min,
    required this.max,
    required this.onChanged,
    required this.format,
  });

  final String label;
  final int value;
  final int min;
  final int max;
  final ValueChanged<int> onChanged;
  final String Function(int) format;

  @override
  Widget build(BuildContext context) {
    final clamped = value.clamp(min, max).toDouble();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _SectionLabel(label),
        Row(
          children: [
            Expanded(
              child: Slider(
                value: clamped,
                min: min.toDouble(),
                max: max.toDouble(),
                divisions: max - min,
                onChanged: (v) => onChanged(v.round()),
              ),
            ),
            _ValueChip(format(value)),
          ],
        ),
      ],
    );
  }
}

class _RangeParamSlider extends StatelessWidget {
  const _RangeParamSlider({
    required this.label,
    required this.lo,
    required this.hi,
    required this.min,
    required this.max,
    required this.onChanged,
    required this.format,
  });

  final String label;
  final int lo;
  final int hi;
  final int min;
  final int max;
  final void Function(int lo, int hi) onChanged;
  final String Function(int) format;

  @override
  Widget build(BuildContext context) {
    final loClamp = lo.clamp(min, max);
    final hiClamp = hi.clamp(loClamp, max);
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _SectionLabel(label),
        Row(
          children: [
            Expanded(
              child: RangeSlider(
                values: RangeValues(loClamp.toDouble(), hiClamp.toDouble()),
                min: min.toDouble(),
                max: max.toDouble(),
                divisions: max - min,
                onChanged: (v) =>
                    onChanged(v.start.round(), v.end.round()),
              ),
            ),
            _ValueChip('${format(loClamp)}–${format(hiClamp)}'),
          ],
        ),
      ],
    );
  }
}

class _BreathingParams extends StatelessWidget {
  const _BreathingParams({
    required this.breathSpeed,
    required this.onBreathSpeed,
  });
  final int breathSpeed;
  final ValueChanged<int> onBreathSpeed;

  @override
  Widget build(BuildContext context) {
    return _ParamSlider(
      label: 'Breath cycle length',
      value: breathSpeed,
      min: 1,
      max: 60,
      onChanged: onBreathSpeed,
      format: (v) => '${v}s',
    );
  }
}

class _PulseParams extends StatelessWidget {
  const _PulseParams({
    required this.pulseSpeed,
    required this.onPulseSpeed,
  });
  final int pulseSpeed;
  final ValueChanged<int> onPulseSpeed;

  @override
  Widget build(BuildContext context) {
    return _ParamSlider(
      label: 'Wave travel time',
      value: pulseSpeed,
      min: 1,
      max: 10,
      onChanged: onPulseSpeed,
      format: (v) => '${v}s',
    );
  }
}

class _ShiftyParams extends StatelessWidget {
  const _ShiftyParams({
    required this.shiftMin,
    required this.shiftMax,
    required this.fade,
    required this.onShiftRange,
    required this.onFade,
  });
  final int shiftMin;
  final int shiftMax;
  final int fade;
  final void Function(int, int) onShiftRange;
  final ValueChanged<int> onFade;

  String _fmtMinutes(int seconds) {
    if (seconds < 60) return '${seconds}s';
    final m = seconds ~/ 60;
    final s = seconds % 60;
    if (s == 0) return '${m}m';
    return '${m}m${s}s';
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _RangeParamSlider(
          label: 'Shift duration',
          lo: shiftMin,
          hi: shiftMax,
          min: 60, // 1 min
          max: 1800, // 30 min
          onChanged: onShiftRange,
          format: _fmtMinutes,
        ),
        _ParamSlider(
          label: 'Fade duration',
          value: fade,
          min: 10,
          max: 300,
          onChanged: onFade,
          format: (v) => '${v}s',
        ),
      ],
    );
  }
}

class _GlitchyParams extends StatelessWidget {
  const _GlitchyParams({
    required this.durMin,
    required this.durMax,
    required this.onRange,
  });
  final int durMin;
  final int durMax;
  final void Function(int, int) onRange;

  @override
  Widget build(BuildContext context) {
    return _RangeParamSlider(
      label: 'Glitch duration (frames @ 30fps)',
      lo: durMin,
      hi: durMax,
      min: 1,
      max: 60,
      onChanged: onRange,
      format: (v) => '$v',
    );
  }
}
