import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';
import '../../control/application/control_notifier.dart';
import '../../control/application/expression_draft.dart';
import '../../control/domain/lamp_color.dart';
import '../../control/domain/sections.dart';
import '../../control/presentation/widgets/color_picker_sheet.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../../control/presentation/widgets/lamp_color_swatch.dart';

const _knownTypes = ['breathing', 'pulse', 'shifty', 'glitchy'];

class ExpressionEditorScreen extends ConsumerStatefulWidget {
  const ExpressionEditorScreen({
    super.key,
    required this.lampId,
    required this.typeKey,
    required this.targetKey,
  });

  /// `_new` for a brand-new expression; otherwise the existing entry's type.
  final String typeKey;

  /// The existing entry's target (1/2/3) — ignored when typeKey == '_new'.
  final int targetKey;

  final String lampId;

  @override
  ConsumerState<ExpressionEditorScreen> createState() =>
      _ExpressionEditorScreenState();
}

class _ExpressionEditorScreenState
    extends ConsumerState<ExpressionEditorScreen> {
  late TextEditingController _parametersCtrl;
  String? _parametersError;
  bool _ctrlSeeded = false;

  bool get _isNew => widget.typeKey == '_new';

  @override
  void initState() {
    super.initState();
    _parametersCtrl = TextEditingController();
  }

  @override
  void dispose() {
    _parametersCtrl.dispose();
    super.dispose();
  }

  /// Seed `_parametersCtrl.text` from the draft the first time we have it.
  /// Subsequent draft changes don't touch the controller (the user is
  /// editing the text directly via onChanged).
  void _seedParametersCtrl(ExpressionConfig draft) {
    if (_ctrlSeeded) return;
    _parametersCtrl.text = jsonEncode(draft.parameters);
    _ctrlSeeded = true;
  }

  void _updateDraft(ExpressionConfig Function(ExpressionConfig d) f) {
    ref
        .read(expressionDraftProvider(
                widget.lampId, widget.typeKey, widget.targetKey)
            .notifier)
        .update(f);
  }

  Map<String, int>? _parseParameters() {
    try {
      final decoded = jsonDecode(_parametersCtrl.text);
      if (decoded is! Map) {
        setState(() => _parametersError = 'Parameters must be a JSON object');
        return null;
      }
      final out = <String, int>{};
      for (final e in decoded.entries) {
        final v = e.value;
        if (v is! num) {
          setState(() =>
              _parametersError = 'Each parameter must be a number');
          return null;
        }
        out[e.key.toString()] = v.toInt();
      }
      setState(() => _parametersError = null);
      return out;
    } catch (_) {
      setState(() => _parametersError = 'Invalid JSON');
      return null;
    }
  }

  @override
  Widget build(BuildContext context) {
    final async = ref.watch(controlNotifierProvider(widget.lampId));
    return Scaffold(
      appBar: AppBar(
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () => GoRouter.maybeOf(context)?.pop(),
          tooltip: 'Back',
        ),
        title: Text(_isNew ? 'New expression' : 'Edit expression'),
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
          _seedParametersCtrl(draft);
          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              // Type
              DropdownButtonFormField<String>(
                initialValue: _knownTypes.contains(draft.type)
                    ? draft.type
                    : (draft.type.isEmpty ? 'breathing' : draft.type),
                decoration: const InputDecoration(
                  labelText: 'Type',
                  border: OutlineInputBorder(),
                ),
                items: _knownTypes
                    .map((t) => DropdownMenuItem(value: t, child: Text(t)))
                    .toList(),
                onChanged: (v) {
                  if (v == null) return;
                  _updateDraft((d) => ExpressionConfig(
                        type: v,
                        enabled: d.enabled,
                        colors: d.colors,
                        intervalMin: d.intervalMin,
                        intervalMax: d.intervalMax,
                        target: d.target,
                        parameters: d.parameters,
                      ));
                },
              ),
              const SizedBox(height: 12),

              // Target
              SegmentedButton<int>(
                showSelectedIcon: false,
                segments: const [
                  ButtonSegment(value: 1, label: Text('Shade')),
                  ButtonSegment(value: 2, label: Text('Base')),
                  ButtonSegment(value: 3, label: Text('Both')),
                ],
                selected: {draft.target},
                onSelectionChanged: (s) =>
                    _updateDraft((d) => ExpressionConfig(
                          type: d.type,
                          enabled: d.enabled,
                          colors: d.colors,
                          intervalMin: d.intervalMin,
                          intervalMax: d.intervalMax,
                          target: s.first,
                          parameters: d.parameters,
                        )),
              ),
              const SizedBox(height: 12),

              // Enabled
              SwitchListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Enabled',
                    style: TextStyle(
                        color: BrandColors.lampWhite, fontSize: 14)),
                value: draft.enabled,
                onChanged: (v) => _updateDraft((d) => ExpressionConfig(
                      type: d.type,
                      enabled: v,
                      colors: d.colors,
                      intervalMin: d.intervalMin,
                      intervalMax: d.intervalMax,
                      target: d.target,
                      parameters: d.parameters,
                    )),
              ),
              const SizedBox(height: 12),

              // Colors
              const Text('Colors',
                  style: TextStyle(
                      color: BrandColors.lampWhite, fontSize: 14)),
              const SizedBox(height: 8),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  for (var i = 0; i < draft.colors.length; i++)
                    GestureDetector(
                      onTap: () async {
                        final picked = await showColorPickerSheet(
                          context,
                          initial: draft.colors[i],
                        );
                        if (picked == null) return;
                        final next = [...draft.colors];
                        next[i] = picked;
                        _updateDraft((d) => ExpressionConfig(
                              type: d.type,
                              enabled: d.enabled,
                              colors: next,
                              intervalMin: d.intervalMin,
                              intervalMax: d.intervalMax,
                              target: d.target,
                              parameters: d.parameters,
                            ));
                      },
                      onLongPress: () => _updateDraft((d) => ExpressionConfig(
                            type: d.type,
                            enabled: d.enabled,
                            colors: [...d.colors]..removeAt(i),
                            intervalMin: d.intervalMin,
                            intervalMax: d.intervalMax,
                            target: d.target,
                            parameters: d.parameters,
                          )),
                      child: LampColorSwatch(color: draft.colors[i], size: 40),
                    ),
                  TextButton.icon(
                    icon: const Icon(Icons.add, size: 18),
                    label: const Text('Add color'),
                    onPressed: () async {
                      final picked = await showColorPickerSheet(
                        context,
                        initial: const LampColor(r: 0xFF, g: 0xFF, b: 0xFF, w: 0),
                      );
                      if (picked == null) return;
                      _updateDraft((d) => ExpressionConfig(
                            type: d.type,
                            enabled: d.enabled,
                            colors: [...d.colors, picked],
                            intervalMin: d.intervalMin,
                            intervalMax: d.intervalMax,
                            target: d.target,
                            parameters: d.parameters,
                          ));
                    },
                  ),
                ],
              ),
              const SizedBox(height: 4),
              const Text('Long-press a swatch to remove it.',
                  style: TextStyle(color: BrandColors.fogGrey, fontSize: 11)),
              const SizedBox(height: 16),

              // Intervals
              const Text('Interval range (seconds)',
                  style: TextStyle(
                      color: BrandColors.lampWhite, fontSize: 14)),
              Row(
                children: [
                  Expanded(
                    child: Slider(
                      min: 10,
                      max: 3600,
                      divisions: 359,
                      value: draft.intervalMin.toDouble().clamp(10, 3600),
                      onChanged: (v) {
                        final newMin = v.round();
                        _updateDraft((d) => ExpressionConfig(
                              type: d.type,
                              enabled: d.enabled,
                              colors: d.colors,
                              intervalMin: newMin,
                              intervalMax:
                                  newMin > d.intervalMax ? newMin : d.intervalMax,
                              target: d.target,
                              parameters: d.parameters,
                            ));
                      },
                    ),
                  ),
                  SizedBox(
                    width: 60,
                    child: Text('${draft.intervalMin}s',
                        textAlign: TextAlign.right,
                        style: const TextStyle(
                            color: BrandColors.fogGrey,
                            fontSize: 12,
                            fontFamily: 'monospace')),
                  ),
                ],
              ),
              Row(
                children: [
                  Expanded(
                    child: Slider(
                      min: 10,
                      max: 3600,
                      divisions: 359,
                      value: draft.intervalMax.toDouble().clamp(10, 3600),
                      onChanged: (v) {
                        final newMax = v.round();
                        _updateDraft((d) => ExpressionConfig(
                              type: d.type,
                              enabled: d.enabled,
                              colors: d.colors,
                              intervalMin:
                                  newMax < d.intervalMin ? newMax : d.intervalMin,
                              intervalMax: newMax,
                              target: d.target,
                              parameters: d.parameters,
                            ));
                      },
                    ),
                  ),
                  SizedBox(
                    width: 60,
                    child: Text('${draft.intervalMax}s',
                        textAlign: TextAlign.right,
                        style: const TextStyle(
                            color: BrandColors.fogGrey,
                            fontSize: 12,
                            fontFamily: 'monospace')),
                  ),
                ],
              ),
              const SizedBox(height: 16),

              // Parameters JSON
              TextField(
                controller: _parametersCtrl,
                maxLines: 3,
                style: const TextStyle(
                    color: BrandColors.lampWhite, fontFamily: 'monospace'),
                decoration: InputDecoration(
                  labelText: 'Parameters (JSON)',
                  hintText: '{"someParam": 100}',
                  border: const OutlineInputBorder(),
                  errorText: _parametersError,
                ),
                onChanged: (_) => _parseParameters(),
              ),
              const SizedBox(height: 24),

              // Actions
              Wrap(
                spacing: 12,
                runSpacing: 12,
                children: [
                  OutlinedButton.icon(
                    icon: const Icon(Icons.play_arrow, size: 18),
                    label: const Text('Test'),
                    onPressed: () async {
                      final params = _parseParameters();
                      if (params == null) return;
                      await notifier.testExpression(ExpressionConfig(
                        type: draft.type,
                        enabled: true,
                        colors: draft.colors,
                        intervalMin: draft.intervalMin,
                        intervalMax: draft.intervalMax,
                        target: draft.target,
                        parameters: params,
                      ));
                    },
                  ),
                  FilledButton.icon(
                    icon: const Icon(Icons.save, size: 18),
                    label: const Text('Save'),
                    onPressed: () async {
                      final params = _parseParameters();
                      if (params == null) return;
                      await notifier.upsertExpression(ExpressionConfig(
                        type: draft.type,
                        enabled: draft.enabled,
                        colors: draft.colors,
                        intervalMin: draft.intervalMin,
                        intervalMax: draft.intervalMax,
                        target: draft.target,
                        parameters: params,
                      ));
                      // Drop the draft so a fresh editor visit reflects
                      // the saved state.
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
                  if (!_isNew)
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
                ],
              ),
            ],
          );
        },
      ),
    );
  }
}
