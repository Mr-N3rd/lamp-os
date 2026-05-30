import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';
import '../../control/application/control_notifier.dart';
import '../../control/application/control_state.dart';
import '../../control/application/expression_draft.dart';
import '../../control/domain/lamp_color.dart';
import '../../control/domain/sections.dart';
import '../../control/presentation/widgets/color_picker_sheet.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../../control/presentation/widgets/lamp_color_swatch.dart';
import '../domain/expression_meta.dart';
import 'widgets/expression_params_panel.dart';

class ExpressionEditorScreen extends ConsumerStatefulWidget {
  const ExpressionEditorScreen({
    super.key,
    required this.lampId,
    required this.typeKey,
    required this.targetKey,
  });

  /// The expression type — locked. New entries reach this screen via the
  /// AddExpressionPickerScreen which chose the type up front; edits
  /// preserve whatever (type, target) the user tapped.
  final String typeKey;

  /// The expression target (1 = shade, 2 = base, 3 = both) — locked.
  final int targetKey;

  final String lampId;

  @override
  ConsumerState<ExpressionEditorScreen> createState() =>
      _ExpressionEditorScreenState();
}

class _ExpressionEditorScreenState
    extends ConsumerState<ExpressionEditorScreen> {
  /// Cached at initState so `dispose()` can call into the notifier without
  /// touching `ref` after the widget is deactivated (which Riverpod blocks).
  late final ControlNotifier _controlNotifier;

  bool _existsInState(ControlState? state) =>
      state != null &&
      state.expressions.expressions
          .any((e) => e.type == widget.typeKey && e.target == widget.targetKey);

  @override
  void initState() {
    super.initState();
    _controlNotifier =
        ref.read(controlNotifierProvider(widget.lampId).notifier);
  }

  @override
  void dispose() {
    // Tell the firmware to leave preview mode and re-enable the configurator
    // behaviors. Without this, a `test_expression` write performed while the
    // editor was open leaves the shade/base configurator stuck in
    // `disabled=true`, so subsequent shade/base color writes are received
    // by the lamp but never drawn.
    _controlNotifier.completeExpressionTest();
    super.dispose();
  }

  void _updateDraft(ExpressionConfig Function(ExpressionConfig d) f) {
    ref
        .read(expressionDraftProvider(
                widget.lampId, widget.typeKey, widget.targetKey)
            .notifier)
        .update(f);
  }

  ExpressionConfig _withColors(ExpressionConfig d, List<LampColor> colors) =>
      ExpressionConfig(
        type: d.type,
        enabled: d.enabled,
        colors: colors,
        intervalMin: d.intervalMin,
        intervalMax: d.intervalMax,
        target: d.target,
        parameters: d.parameters,
      );

  ExpressionConfig _withIntervals(ExpressionConfig d, int min, int max) =>
      ExpressionConfig(
        type: d.type,
        enabled: d.enabled,
        colors: d.colors,
        intervalMin: min,
        intervalMax: max,
        target: d.target,
        parameters: d.parameters,
      );

  ExpressionConfig _withParameters(
          ExpressionConfig d, Map<String, int> p) =>
      ExpressionConfig(
        type: d.type,
        enabled: d.enabled,
        colors: d.colors,
        intervalMin: d.intervalMin,
        intervalMax: d.intervalMax,
        target: d.target,
        parameters: p,
      );

  @override
  Widget build(BuildContext context) {
    final async = ref.watch(controlNotifierProvider(widget.lampId));
    final meta = ExpressionTypeMeta.byKey(widget.typeKey);
    return Scaffold(
      appBar: AppBar(
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () {
            final router = GoRouter.maybeOf(context);
            if (router != null && router.canPop()) {
              router.pop();
            } else {
              Navigator.maybeOf(context)?.maybePop();
            }
          },
          tooltip: 'Back',
        ),
        // Title carries the expression's friendly name (titlized via
        // ExpressionTypeMeta) rather than the wire-level snake-case type.
        title: Text(_existsInState(async.value)
            ? (meta?.name ?? widget.typeKey)
            : 'New ${meta?.name ?? widget.typeKey}'),
        actions: [
          // Context action in the AppBar — `Test` is the only one-shot,
          // non-destructive action specific to this screen. Pattern: page-
          // specific test/run actions live here; Save and Delete live in
          // the page body so they aren't easy to mistap.
          if (async.value != null)
            _AppBarTestAction(
              onPressed: () async {
                final draft = ref.read(expressionDraftProvider(
                    widget.lampId, widget.typeKey, widget.targetKey));
                final notifier =
                    ref.read(controlNotifierProvider(widget.lampId).notifier);
                await notifier.testExpression(ExpressionConfig(
                  type: draft.type,
                  enabled: true,
                  colors: draft.colors,
                  intervalMin: draft.intervalMin,
                  intervalMax: draft.intervalMax,
                  target: draft.target,
                  parameters: draft.parameters,
                ));
              },
            ),
        ],
      ),
      body: async.when(
        loading: () => ConnectingView(deviceId: widget.lampId),
        error: (e, _) => Center(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Text(
              'Could not reach this lamp: $e',
              style: const TextStyle(color: BrandColors.fogGrey),
              textAlign: TextAlign.center,
            ),
          ),
        ),
        data: (state) {
          final notifier =
              ref.read(controlNotifierProvider(widget.lampId).notifier);
          final draft = ref.watch(expressionDraftProvider(
              widget.lampId, widget.typeKey, widget.targetKey));
          final isNew = !_existsInState(state);

          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              _Header(meta: meta, target: widget.targetKey),
              const SizedBox(height: 20),

              // Colors
              const _Label('Colors'),
              const SizedBox(height: 8),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  for (var i = 0; i < draft.colors.length; i++)
                    _ColorChip(
                      color: draft.colors[i],
                      onEdit: () async {
                        final picked = await showColorPickerSheet(
                          context,
                          initial: draft.colors[i],
                        );
                        if (picked == null) return;
                        final next = [...draft.colors];
                        next[i] = picked;
                        _updateDraft((d) => _withColors(d, next));
                      },
                      onRemove: () => _updateDraft(
                          (d) => _withColors(d, [...d.colors]..removeAt(i))),
                    ),
                  TextButton.icon(
                    icon: const Icon(Icons.add, size: 18),
                    label: const Text('Add color'),
                    onPressed: () async {
                      final picked = await showColorPickerSheet(
                        context,
                        initial:
                            const LampColor(r: 0xFF, g: 0xFF, b: 0xFF, w: 0),
                      );
                      if (picked == null) return;
                      _updateDraft((d) => _withColors(d, [...d.colors, picked]));
                    },
                  ),
                ],
              ),
              const SizedBox(height: 16),

              // Trigger cadence — two sliders that hide the seconds math.
              // "Frequency" is the center (geometric mean of min/max) on a
              // rare↔often axis. "Predictability" widens the random spread
              // around that center.
              _FrequencySpread(
                intervalMin: draft.intervalMin,
                intervalMax: draft.intervalMax,
                onChanged: (lo, hi) =>
                    _updateDraft((d) => _withIntervals(d, lo, hi)),
              ),
              const SizedBox(height: 16),

              // Per-type parameters (replaces the old JSON text field).
              ExpressionParamsPanel(
                type: draft.type,
                parameters: draft.parameters,
                onChanged: (p) => _updateDraft((d) => _withParameters(d, p)),
              ),
              const SizedBox(height: 24),

              // Action row — Delete on the left, Save on the right.
              // Cancel/back lives on the AppBar's leading arrow. Test is on
              // the AppBar actions area. Pattern for all editable panes.
              Row(
                children: [
                  if (!isNew)
                    TextButton.icon(
                      icon: const Icon(Icons.delete, size: 18),
                      label: const Text('Delete'),
                      style: TextButton.styleFrom(
                          foregroundColor: Colors.redAccent),
                      onPressed: () async {
                        await notifier.removeExpression(
                          type: widget.typeKey,
                          target: widget.targetKey,
                        );
                        ref
                            .read(expressionDraftProvider(widget.lampId,
                                    widget.typeKey, widget.targetKey)
                                .notifier)
                            .reset();
                        if (context.mounted) {
                          GoRouter.maybeOf(context)?.pop();
                        }
                      },
                    ),
                  const Spacer(),
                  FilledButton.icon(
                    icon: const Icon(Icons.save, size: 18),
                    label: const Text('Save'),
                    onPressed: () async {
                      await notifier.upsertExpression(ExpressionConfig(
                        type: draft.type,
                        enabled: draft.enabled,
                        colors: draft.colors,
                        intervalMin: draft.intervalMin,
                        intervalMax: draft.intervalMax,
                        target: draft.target,
                        parameters: draft.parameters,
                      ));
                      ref
                          .read(expressionDraftProvider(widget.lampId,
                                  widget.typeKey, widget.targetKey)
                              .notifier)
                          .reset();
                      if (context.mounted) {
                        GoRouter.maybeOf(context)?.pop();
                      }
                    },
                  ),
                ],
              ),
            ],
          );
        },
      ),
    );
  }
}

class _Label extends StatelessWidget {
  const _Label(this.text);
  final String text;

  @override
  Widget build(BuildContext context) {
    return Text(
      text,
      style: const TextStyle(color: BrandColors.lampWhite, fontSize: 14),
    );
  }
}

class _Header extends StatelessWidget {
  const _Header({required this.meta, required this.target});
  final ExpressionTypeMeta? meta;
  final int target;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: BrandColors.lampWhite.withValues(alpha: 0.04),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(
          color: BrandColors.lampWhite.withValues(alpha: 0.06),
        ),
      ),
      child: Row(
        children: [
          if (meta != null)
            Container(
              width: 40,
              height: 40,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: BrandColors.auroraBlue.withValues(alpha: 0.18),
              ),
              child: Icon(meta!.icon, color: BrandColors.auroraBlue),
            ),
          if (meta != null) const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  meta?.name ?? '(unknown)',
                  style: const TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 16,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                Text(
                  'Target: ${targetLabel(target)}',
                  style: const TextStyle(
                    color: BrandColors.fogGrey,
                    fontSize: 12,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// Pill-shaped, AppBar-tinted "Test" action. Placed in `AppBar.actions` per
/// the agreed pattern: page-specific, non-destructive one-shot actions live
/// in the title area; Save and Delete sit in the page body where they're
/// harder to mistap.
class _AppBarTestAction extends StatelessWidget {
  const _AppBarTestAction({required this.onPressed});
  final Future<void> Function() onPressed;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
      child: TextButton.icon(
        onPressed: onPressed,
        icon: const Icon(Icons.play_arrow, size: 18),
        label: const Text('Test'),
        style: TextButton.styleFrom(
          foregroundColor: BrandColors.auroraBlue,
          shape: const StadiumBorder(),
        ),
      ),
    );
  }
}

/// Two-slider rendering of the interval range that hides the seconds math:
///   - Frequency (rare ↔ often): log-mapped centre of the range.
///   - Predictability (predictable ↔ varied): width of the spread around
///     that centre.
///
/// We round-trip through (intervalMin, intervalMax) so the firmware
/// contract is unchanged. Each slider position is held in local state
/// (`_freq` / `_spread`) so moving one slider doesn't shift the other's
/// position via re-derivation from clamped min/max. The local positions
/// only re-sync from the upstream min/max when the user opens an
/// existing expression — see `didUpdateWidget`.
class _FrequencySpread extends StatefulWidget {
  const _FrequencySpread({
    required this.intervalMin,
    required this.intervalMax,
    required this.onChanged,
  });

  final int intervalMin;
  final int intervalMax;
  final void Function(int min, int max) onChanged;

  // Firmware allows 10s … 3600s.
  static const int minSec = 10;
  static const int maxSec = 3600;
  // Spread multiplier range. spread=0 → min == max. spread=1 → [c/5, c*5].
  static const double maxMultiplier = 5.0;

  @override
  State<_FrequencySpread> createState() => _FrequencySpreadState();
}

class _FrequencySpreadState extends State<_FrequencySpread> {
  late double _freq;
  late double _spread;

  @override
  void initState() {
    super.initState();
    final centre = _centreOf(widget.intervalMin, widget.intervalMax);
    _freq = _freqFromCentre(centre);
    _spread = _spreadFromMultiplier(
        _multOf(widget.intervalMin, widget.intervalMax));
  }

  @override
  void didUpdateWidget(_FrequencySpread oldWidget) {
    super.didUpdateWidget(oldWidget);
    // Only re-sync from the upstream `(intervalMin, intervalMax)` when the
    // parent value would land us on a meaningfully different slider
    // position than where the user currently has it. Otherwise we'd
    // bounce the slider thumb under the user's finger on every emit.
    final centre = _centreOf(widget.intervalMin, widget.intervalMax);
    final upstreamFreq = _freqFromCentre(centre);
    final upstreamSpread = _spreadFromMultiplier(
        _multOf(widget.intervalMin, widget.intervalMax));
    if ((upstreamFreq - _freq).abs() > 0.02) _freq = upstreamFreq;
    if ((upstreamSpread - _spread).abs() > 0.02) _spread = upstreamSpread;
  }

  // ---- Frequency (0 = rare/hourly, 1 = often/10s) ----

  static double _freqFromCentre(double centreSec) {
    final lo = math.log(_FrequencySpread.minSec.toDouble());
    final hi = math.log(_FrequencySpread.maxSec.toDouble());
    final t = (math.log(centreSec) - hi) / (lo - hi);
    return t.clamp(0.0, 1.0);
  }

  static double _centreFromFreq(double freq) {
    final lo = math.log(_FrequencySpread.minSec.toDouble());
    final hi = math.log(_FrequencySpread.maxSec.toDouble());
    return math.exp(hi + (lo - hi) * freq.clamp(0.0, 1.0));
  }

  // ---- Spread (0 = identical/predictable, 1 = full 5× spread) ----

  static double _spreadFromMultiplier(double mult) {
    return ((mult - 1.0) / (_FrequencySpread.maxMultiplier - 1.0))
        .clamp(0.0, 1.0);
  }

  static double _multiplierFromSpread(double spread) {
    return 1.0 +
        spread.clamp(0.0, 1.0) *
            (_FrequencySpread.maxMultiplier - 1.0);
  }

  // ---- Decode existing min/max → (centre, multiplier) ----

  static double _centreOf(int min, int max) =>
      math.sqrt(min.toDouble() * max.toDouble());

  static double _multOf(int min, int max) =>
      max == 0 || min == 0 ? 1.0 : math.sqrt(max / min);

  static String _fmt(double seconds) {
    if (seconds < 1) return '${(seconds * 1000).round()}ms';
    if (seconds < 90) return '${seconds.round()}s';
    final m = seconds / 60;
    if (m < 90) return '${m.round()}m';
    final h = m / 60;
    return '${h.toStringAsFixed(1).replaceAll(RegExp(r'\.0$'), '')}h';
  }

  void _emit() {
    final centre = _centreFromFreq(_freq);
    final mult = _multiplierFromSpread(_spread);
    final lo = (centre / mult).clamp(_FrequencySpread.minSec.toDouble(),
        _FrequencySpread.maxSec.toDouble());
    final hi = (centre * mult).clamp(_FrequencySpread.minSec.toDouble(),
        _FrequencySpread.maxSec.toDouble());
    widget.onChanged(lo.round(), hi.round());
  }

  @override
  Widget build(BuildContext context) {
    final centre = _centreFromFreq(_freq);
    final mult = _multiplierFromSpread(_spread);
    final lo = (centre / mult).clamp(_FrequencySpread.minSec.toDouble(),
        _FrequencySpread.maxSec.toDouble());
    final hi = (centre * mult).clamp(_FrequencySpread.minSec.toDouble(),
        _FrequencySpread.maxSec.toDouble());

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        const Padding(
          padding: EdgeInsets.only(top: 8, bottom: 2),
          child: Text(
            'Frequency',
            style:
                TextStyle(color: BrandColors.lampWhite, fontSize: 14),
          ),
        ),
        Row(
          children: [
            const Text('rare',
                style: TextStyle(
                    color: BrandColors.fogGrey, fontSize: 11)),
            Expanded(
              child: Slider(
                value: _freq,
                min: 0,
                max: 1,
                onChanged: (v) {
                  setState(() => _freq = v);
                  _emit();
                },
              ),
            ),
            const Text('often',
                style: TextStyle(
                    color: BrandColors.fogGrey, fontSize: 11)),
          ],
        ),
        const Padding(
          padding: EdgeInsets.only(top: 12, bottom: 2),
          child: Text(
            'Predictability',
            style:
                TextStyle(color: BrandColors.lampWhite, fontSize: 14),
          ),
        ),
        Row(
          children: [
            const Text('exact',
                style: TextStyle(
                    color: BrandColors.fogGrey, fontSize: 11)),
            Expanded(
              child: Slider(
                value: _spread,
                min: 0,
                max: 1,
                onChanged: (v) {
                  setState(() => _spread = v);
                  _emit();
                },
              ),
            ),
            const Text('varied',
                style: TextStyle(
                    color: BrandColors.fogGrey, fontSize: 11)),
          ],
        ),
        Padding(
          padding: const EdgeInsets.only(top: 6),
          child: Text(
            _spread < 0.02
                ? 'Roughly every ${_fmt(centre)}.'
                : 'Between ${_fmt(lo)} and ${_fmt(hi)}.',
            style: const TextStyle(
              color: BrandColors.fogGrey,
              fontSize: 11,
            ),
          ),
        ),
      ],
    );
  }
}

class _ColorChip extends StatelessWidget {
  const _ColorChip({
    required this.color,
    required this.onEdit,
    required this.onRemove,
  });

  final LampColor color;
  final VoidCallback onEdit;
  final VoidCallback onRemove;

  @override
  Widget build(BuildContext context) {
    return Stack(
      clipBehavior: Clip.none,
      children: [
        GestureDetector(
          onTap: onEdit,
          child: LampColorSwatch(color: color, size: 40),
        ),
        Positioned(
          top: -6,
          right: -6,
          child: Semantics(
            label: 'Remove color',
            button: true,
            child: InkWell(
              onTap: onRemove,
              customBorder: const CircleBorder(),
              child: Container(
                width: 18,
                height: 18,
                decoration: const BoxDecoration(
                  color: BrandColors.ashGrey,
                  shape: BoxShape.circle,
                ),
                child: const Icon(
                  Icons.close,
                  size: 12,
                  color: BrandColors.lampWhite,
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }
}
