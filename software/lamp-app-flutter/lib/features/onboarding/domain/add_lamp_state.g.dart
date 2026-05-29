// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'add_lamp_state.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

_AddLampState _$AddLampStateFromJson(Map<String, dynamic> json) =>
    _AddLampState(
      step:
          $enumDecodeNullable(_$AddLampStepEnumMap, json['step']) ??
          AddLampStep.scan,
      deviceId: json['deviceId'] as String? ?? '',
      name: json['name'] as String? ?? '',
      password: json['password'] as String? ?? '',
      status:
          $enumDecodeNullable(_$AddLampStatusEnumMap, json['status']) ??
          AddLampStatus.idle,
      errorMessage: json['errorMessage'] as String?,
    );

Map<String, dynamic> _$AddLampStateToJson(_AddLampState instance) =>
    <String, dynamic>{
      'step': _$AddLampStepEnumMap[instance.step]!,
      'deviceId': instance.deviceId,
      'name': instance.name,
      'password': instance.password,
      'status': _$AddLampStatusEnumMap[instance.status]!,
      'errorMessage': instance.errorMessage,
    };

const _$AddLampStepEnumMap = {
  AddLampStep.scan: 'scan',
  AddLampStep.name: 'name',
  AddLampStep.password: 'password',
  AddLampStep.done: 'done',
};

const _$AddLampStatusEnumMap = {
  AddLampStatus.idle: 'idle',
  AddLampStatus.working: 'working',
  AddLampStatus.error: 'error',
};
