import 'package:riverpod_annotation/riverpod_annotation.dart';

import '../domain/sections.dart';
import 'control_notifier.dart';

part 'expression_draft.g.dart';

/// Working copy of an expression while the user is editing it. Held in a
/// keep-alive provider so navigating away from the editor and back doesn't
/// lose in-progress edits. Save / Delete invalidate the entry.
///
/// Family key is `(lampId, type, target)`. The sentinel `_new` is used by
/// the FAB "+ Add expression" entry point; for that key, an empty draft is
/// returned. For all other keys we copy the matching existing entry out of
/// `controlNotifierProvider`'s `expressions.expressions`.
@Riverpod(keepAlive: true, name: 'expressionDraftProvider')
class ExpressionDraft extends _$ExpressionDraft {
  @override
  ExpressionConfig build(String lampId, String type, int target) {
    if (type == '_new') return _defaultDraft();
    final state = ref.read(controlNotifierProvider(lampId)).value;
    if (state == null) return _defaultDraft();
    return state.expressions.expressions.firstWhere(
      (e) => e.type == type && e.target == target,
      orElse: _defaultDraft,
    );
  }

  /// Replace the draft with the result of [f] applied to the current value.
  void update(ExpressionConfig Function(ExpressionConfig current) f) =>
      state = f(state);

  /// Drop the draft so the next [build] gets a fresh copy. Call after Save
  /// or Delete to avoid leaking stale state into the next editor visit.
  void reset() {
    ref.invalidate(expressionDraftProvider(lampId, type, target));
  }

  static ExpressionConfig _defaultDraft() => const ExpressionConfig(
        type: 'breathing',
        enabled: true,
        colors: [],
        intervalMin: 60,
        intervalMax: 900,
        target: 3,
        parameters: {},
      );
}
