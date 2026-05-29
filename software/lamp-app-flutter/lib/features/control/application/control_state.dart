import 'package:freezed_annotation/freezed_annotation.dart';

import '../domain/sections.dart';

part 'control_state.freezed.dart';

/// Combined state for the Control screen. Populated by ControlNotifier after
/// connect+auth+per-section reads. Not JSON-serialized — purely in-memory.
@freezed
abstract class ControlState with _$ControlState {
  const factory ControlState({
    required LampSection lamp,
    required BaseSection base,
    required ShadeSection shade,
  }) = _ControlState;
}
