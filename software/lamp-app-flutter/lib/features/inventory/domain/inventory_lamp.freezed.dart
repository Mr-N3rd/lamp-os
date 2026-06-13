// GENERATED CODE - DO NOT MODIFY BY HAND
// coverage:ignore-file
// ignore_for_file: type=lint
// ignore_for_file: unused_element, deprecated_member_use, deprecated_member_use_from_same_package, use_function_type_syntax_for_parameters, unnecessary_const, avoid_init_to_null, invalid_override_different_default_values_named, prefer_expression_function_bodies, annotate_overrides, invalid_annotation_target, unnecessary_question_mark

part of 'inventory_lamp.dart';

// **************************************************************************
// FreezedGenerator
// **************************************************************************

// dart format off
T _$identity<T>(T value) => value;

/// @nodoc
mixin _$InventoryLamp {

 String get id; String get name; String? get controlPassword; int? get lastSeenEpochMs; List<int>? get lastShadeColor; List<int>? get lastBaseColor;
/// Create a copy of InventoryLamp
/// with the given fields replaced by the non-null parameter values.
@JsonKey(includeFromJson: false, includeToJson: false)
@pragma('vm:prefer-inline')
$InventoryLampCopyWith<InventoryLamp> get copyWith => _$InventoryLampCopyWithImpl<InventoryLamp>(this as InventoryLamp, _$identity);

  /// Serializes this InventoryLamp to a JSON map.
  Map<String, dynamic> toJson();


@override
bool operator ==(Object other) {
  return identical(this, other) || (other.runtimeType == runtimeType&&other is InventoryLamp&&(identical(other.id, id) || other.id == id)&&(identical(other.name, name) || other.name == name)&&(identical(other.controlPassword, controlPassword) || other.controlPassword == controlPassword)&&(identical(other.lastSeenEpochMs, lastSeenEpochMs) || other.lastSeenEpochMs == lastSeenEpochMs)&&const DeepCollectionEquality().equals(other.lastShadeColor, lastShadeColor)&&const DeepCollectionEquality().equals(other.lastBaseColor, lastBaseColor));
}

@JsonKey(includeFromJson: false, includeToJson: false)
@override
int get hashCode => Object.hash(runtimeType,id,name,controlPassword,lastSeenEpochMs,const DeepCollectionEquality().hash(lastShadeColor),const DeepCollectionEquality().hash(lastBaseColor));

@override
String toString() {
  return 'InventoryLamp(id: $id, name: $name, controlPassword: $controlPassword, lastSeenEpochMs: $lastSeenEpochMs, lastShadeColor: $lastShadeColor, lastBaseColor: $lastBaseColor)';
}


}

/// @nodoc
abstract mixin class $InventoryLampCopyWith<$Res>  {
  factory $InventoryLampCopyWith(InventoryLamp value, $Res Function(InventoryLamp) _then) = _$InventoryLampCopyWithImpl;
@useResult
$Res call({
 String id, String name, String? controlPassword, int? lastSeenEpochMs, List<int>? lastShadeColor, List<int>? lastBaseColor
});




}
/// @nodoc
class _$InventoryLampCopyWithImpl<$Res>
    implements $InventoryLampCopyWith<$Res> {
  _$InventoryLampCopyWithImpl(this._self, this._then);

  final InventoryLamp _self;
  final $Res Function(InventoryLamp) _then;

/// Create a copy of InventoryLamp
/// with the given fields replaced by the non-null parameter values.
@pragma('vm:prefer-inline') @override $Res call({Object? id = null,Object? name = null,Object? controlPassword = freezed,Object? lastSeenEpochMs = freezed,Object? lastShadeColor = freezed,Object? lastBaseColor = freezed,}) {
  return _then(_self.copyWith(
id: null == id ? _self.id : id // ignore: cast_nullable_to_non_nullable
as String,name: null == name ? _self.name : name // ignore: cast_nullable_to_non_nullable
as String,controlPassword: freezed == controlPassword ? _self.controlPassword : controlPassword // ignore: cast_nullable_to_non_nullable
as String?,lastSeenEpochMs: freezed == lastSeenEpochMs ? _self.lastSeenEpochMs : lastSeenEpochMs // ignore: cast_nullable_to_non_nullable
as int?,lastShadeColor: freezed == lastShadeColor ? _self.lastShadeColor : lastShadeColor // ignore: cast_nullable_to_non_nullable
as List<int>?,lastBaseColor: freezed == lastBaseColor ? _self.lastBaseColor : lastBaseColor // ignore: cast_nullable_to_non_nullable
as List<int>?,
  ));
}

}


/// Adds pattern-matching-related methods to [InventoryLamp].
extension InventoryLampPatterns on InventoryLamp {
/// A variant of `map` that fallback to returning `orElse`.
///
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case final Subclass value:
///     return ...;
///   case _:
///     return orElse();
/// }
/// ```

@optionalTypeArgs TResult maybeMap<TResult extends Object?>(TResult Function( _InventoryLamp value)?  $default,{required TResult orElse(),}){
final _that = this;
switch (_that) {
case _InventoryLamp() when $default != null:
return $default(_that);case _:
  return orElse();

}
}
/// A `switch`-like method, using callbacks.
///
/// Callbacks receives the raw object, upcasted.
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case final Subclass value:
///     return ...;
///   case final Subclass2 value:
///     return ...;
/// }
/// ```

@optionalTypeArgs TResult map<TResult extends Object?>(TResult Function( _InventoryLamp value)  $default,){
final _that = this;
switch (_that) {
case _InventoryLamp():
return $default(_that);case _:
  throw StateError('Unexpected subclass');

}
}
/// A variant of `map` that fallback to returning `null`.
///
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case final Subclass value:
///     return ...;
///   case _:
///     return null;
/// }
/// ```

@optionalTypeArgs TResult? mapOrNull<TResult extends Object?>(TResult? Function( _InventoryLamp value)?  $default,){
final _that = this;
switch (_that) {
case _InventoryLamp() when $default != null:
return $default(_that);case _:
  return null;

}
}
/// A variant of `when` that fallback to an `orElse` callback.
///
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case Subclass(:final field):
///     return ...;
///   case _:
///     return orElse();
/// }
/// ```

@optionalTypeArgs TResult maybeWhen<TResult extends Object?>(TResult Function( String id,  String name,  String? controlPassword,  int? lastSeenEpochMs,  List<int>? lastShadeColor,  List<int>? lastBaseColor)?  $default,{required TResult orElse(),}) {final _that = this;
switch (_that) {
case _InventoryLamp() when $default != null:
return $default(_that.id,_that.name,_that.controlPassword,_that.lastSeenEpochMs,_that.lastShadeColor,_that.lastBaseColor);case _:
  return orElse();

}
}
/// A `switch`-like method, using callbacks.
///
/// As opposed to `map`, this offers destructuring.
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case Subclass(:final field):
///     return ...;
///   case Subclass2(:final field2):
///     return ...;
/// }
/// ```

@optionalTypeArgs TResult when<TResult extends Object?>(TResult Function( String id,  String name,  String? controlPassword,  int? lastSeenEpochMs,  List<int>? lastShadeColor,  List<int>? lastBaseColor)  $default,) {final _that = this;
switch (_that) {
case _InventoryLamp():
return $default(_that.id,_that.name,_that.controlPassword,_that.lastSeenEpochMs,_that.lastShadeColor,_that.lastBaseColor);case _:
  throw StateError('Unexpected subclass');

}
}
/// A variant of `when` that fallback to returning `null`
///
/// It is equivalent to doing:
/// ```dart
/// switch (sealedClass) {
///   case Subclass(:final field):
///     return ...;
///   case _:
///     return null;
/// }
/// ```

@optionalTypeArgs TResult? whenOrNull<TResult extends Object?>(TResult? Function( String id,  String name,  String? controlPassword,  int? lastSeenEpochMs,  List<int>? lastShadeColor,  List<int>? lastBaseColor)?  $default,) {final _that = this;
switch (_that) {
case _InventoryLamp() when $default != null:
return $default(_that.id,_that.name,_that.controlPassword,_that.lastSeenEpochMs,_that.lastShadeColor,_that.lastBaseColor);case _:
  return null;

}
}

}

/// @nodoc
@JsonSerializable()

class _InventoryLamp implements InventoryLamp {
  const _InventoryLamp({required this.id, required this.name, this.controlPassword, this.lastSeenEpochMs, final  List<int>? lastShadeColor, final  List<int>? lastBaseColor}): _lastShadeColor = lastShadeColor,_lastBaseColor = lastBaseColor;
  factory _InventoryLamp.fromJson(Map<String, dynamic> json) => _$InventoryLampFromJson(json);

@override final  String id;
@override final  String name;
@override final  String? controlPassword;
@override final  int? lastSeenEpochMs;
 final  List<int>? _lastShadeColor;
@override List<int>? get lastShadeColor {
  final value = _lastShadeColor;
  if (value == null) return null;
  if (_lastShadeColor is EqualUnmodifiableListView) return _lastShadeColor;
  // ignore: implicit_dynamic_type
  return EqualUnmodifiableListView(value);
}

 final  List<int>? _lastBaseColor;
@override List<int>? get lastBaseColor {
  final value = _lastBaseColor;
  if (value == null) return null;
  if (_lastBaseColor is EqualUnmodifiableListView) return _lastBaseColor;
  // ignore: implicit_dynamic_type
  return EqualUnmodifiableListView(value);
}


/// Create a copy of InventoryLamp
/// with the given fields replaced by the non-null parameter values.
@override @JsonKey(includeFromJson: false, includeToJson: false)
@pragma('vm:prefer-inline')
_$InventoryLampCopyWith<_InventoryLamp> get copyWith => __$InventoryLampCopyWithImpl<_InventoryLamp>(this, _$identity);

@override
Map<String, dynamic> toJson() {
  return _$InventoryLampToJson(this, );
}

@override
bool operator ==(Object other) {
  return identical(this, other) || (other.runtimeType == runtimeType&&other is _InventoryLamp&&(identical(other.id, id) || other.id == id)&&(identical(other.name, name) || other.name == name)&&(identical(other.controlPassword, controlPassword) || other.controlPassword == controlPassword)&&(identical(other.lastSeenEpochMs, lastSeenEpochMs) || other.lastSeenEpochMs == lastSeenEpochMs)&&const DeepCollectionEquality().equals(other._lastShadeColor, _lastShadeColor)&&const DeepCollectionEquality().equals(other._lastBaseColor, _lastBaseColor));
}

@JsonKey(includeFromJson: false, includeToJson: false)
@override
int get hashCode => Object.hash(runtimeType,id,name,controlPassword,lastSeenEpochMs,const DeepCollectionEquality().hash(_lastShadeColor),const DeepCollectionEquality().hash(_lastBaseColor));

@override
String toString() {
  return 'InventoryLamp(id: $id, name: $name, controlPassword: $controlPassword, lastSeenEpochMs: $lastSeenEpochMs, lastShadeColor: $lastShadeColor, lastBaseColor: $lastBaseColor)';
}


}

/// @nodoc
abstract mixin class _$InventoryLampCopyWith<$Res> implements $InventoryLampCopyWith<$Res> {
  factory _$InventoryLampCopyWith(_InventoryLamp value, $Res Function(_InventoryLamp) _then) = __$InventoryLampCopyWithImpl;
@override @useResult
$Res call({
 String id, String name, String? controlPassword, int? lastSeenEpochMs, List<int>? lastShadeColor, List<int>? lastBaseColor
});




}
/// @nodoc
class __$InventoryLampCopyWithImpl<$Res>
    implements _$InventoryLampCopyWith<$Res> {
  __$InventoryLampCopyWithImpl(this._self, this._then);

  final _InventoryLamp _self;
  final $Res Function(_InventoryLamp) _then;

/// Create a copy of InventoryLamp
/// with the given fields replaced by the non-null parameter values.
@override @pragma('vm:prefer-inline') $Res call({Object? id = null,Object? name = null,Object? controlPassword = freezed,Object? lastSeenEpochMs = freezed,Object? lastShadeColor = freezed,Object? lastBaseColor = freezed,}) {
  return _then(_InventoryLamp(
id: null == id ? _self.id : id // ignore: cast_nullable_to_non_nullable
as String,name: null == name ? _self.name : name // ignore: cast_nullable_to_non_nullable
as String,controlPassword: freezed == controlPassword ? _self.controlPassword : controlPassword // ignore: cast_nullable_to_non_nullable
as String?,lastSeenEpochMs: freezed == lastSeenEpochMs ? _self.lastSeenEpochMs : lastSeenEpochMs // ignore: cast_nullable_to_non_nullable
as int?,lastShadeColor: freezed == lastShadeColor ? _self._lastShadeColor : lastShadeColor // ignore: cast_nullable_to_non_nullable
as List<int>?,lastBaseColor: freezed == lastBaseColor ? _self._lastBaseColor : lastBaseColor // ignore: cast_nullable_to_non_nullable
as List<int>?,
  ));
}


}

// dart format on
