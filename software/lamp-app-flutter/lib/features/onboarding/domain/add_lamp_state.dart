import 'package:freezed_annotation/freezed_annotation.dart';

part 'add_lamp_state.freezed.dart';
part 'add_lamp_state.g.dart';

enum AddLampStep { scan, name, wifi, done }

enum AddLampStatus { idle, working, error }

@freezed
abstract class AddLampState with _$AddLampState {
  const factory AddLampState({
    @Default(AddLampStep.scan) AddLampStep step,
    @Default('') String deviceId,
    @Default('') String name,
    @Default('') String ssid,
    @Default('') String password,
    @Default(AddLampStatus.idle) AddLampStatus status,
    String? errorMessage,
  }) = _AddLampState;

  factory AddLampState.fromJson(Map<String, dynamic> json) =>
      _$AddLampStateFromJson(json);
}
