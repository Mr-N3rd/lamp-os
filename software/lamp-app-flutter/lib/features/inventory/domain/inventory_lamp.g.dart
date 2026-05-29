// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'inventory_lamp.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

_InventoryLamp _$InventoryLampFromJson(Map<String, dynamic> json) =>
    _InventoryLamp(
      id: json['id'] as String,
      name: json['name'] as String,
      lastSeenEpochMs: (json['lastSeenEpochMs'] as num?)?.toInt(),
      lastShadeColor: (json['lastShadeColor'] as List<dynamic>?)
          ?.map((e) => (e as num).toInt())
          .toList(),
      lastBaseColor: (json['lastBaseColor'] as List<dynamic>?)
          ?.map((e) => (e as num).toInt())
          .toList(),
    );

Map<String, dynamic> _$InventoryLampToJson(_InventoryLamp instance) =>
    <String, dynamic>{
      'id': instance.id,
      'name': instance.name,
      'lastSeenEpochMs': instance.lastSeenEpochMs,
      'lastShadeColor': instance.lastShadeColor,
      'lastBaseColor': instance.lastBaseColor,
    };
