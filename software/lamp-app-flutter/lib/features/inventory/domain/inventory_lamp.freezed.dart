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

 String get id; String get name; String? get controlPassword;/// Persistent random critter pick (1-8) assigned at adopt/add time so
/// each lamp keeps the same critter friend across sessions and across
/// the connecting/preview surfaces. Nullable for legacy entries adopted
/// before this field existed — consumers fall back to a deviceId hash.
 int? get critterIndex; int? get lastSeenEpochMs;/// Cached last-seen colors written by `controlNotifier._updateSeen`
/// on every successful connect-and-read and every settled slider
/// drag. Persisted via `inventory.v1` in SharedPreferences and read
/// back by `resolveLampColors` to render My Lamps / picker tiles.
///
/// Shape: `[R, G, B, W]` (4 ints). Legacy entries written before
/// this field grew the W byte may be `[R, G, B]` (length 3) — the
/// resolver treats those as `W = 0`, preserving the prior render.
 List<int>? get lastShadeColor; List<int>? get lastBaseColor;/// Last observed `isMesh` (capability bit 1 in the adv) — true when
/// the lamp speaks the app's v0x03 mesh protocol, false for legacy
/// BT-only firmware. Set by `nearby_lamps_notifier` whenever a fresh
/// adv arrives. `lampRouteResolver` reads this when the live roster
/// is empty (lamp out of range) so an offline legacy BT-only lamp
/// routes to BtOnlyLampScreen instead of stranding the user on
/// ConnectingView forever. Nullable for legacy inventory entries
/// written before this field existed — resolvers default to
/// "assume mesh-capable" for those, mirroring the pre-fix behavior.
 bool? get lastKnownIsMesh;/// GATT-discovered presence of CHAR_COMMIT (Phase A firmware).
/// Populated post-connect by `inventory_notifier.updateHasCommitChar`.
/// Null when not yet probed (pre-existing inventory entries, or the
/// mid-connect window before the probe resolves). Read sites MUST
/// default null to false via `?? false` — null means "unknown", and
/// unknown should fall back to the legacy Save-pill flow rather than
/// incorrectly enabling Phase A behavior.
 bool? get hasCommitChar;
/// Create a copy of InventoryLamp
/// with the given fields replaced by the non-null parameter values.
@JsonKey(includeFromJson: false, includeToJson: false)
@pragma('vm:prefer-inline')
$InventoryLampCopyWith<InventoryLamp> get copyWith => _$InventoryLampCopyWithImpl<InventoryLamp>(this as InventoryLamp, _$identity);

  /// Serializes this InventoryLamp to a JSON map.
  Map<String, dynamic> toJson();


@override
bool operator ==(Object other) {
  return identical(this, other) || (other.runtimeType == runtimeType&&other is InventoryLamp&&(identical(other.id, id) || other.id == id)&&(identical(other.name, name) || other.name == name)&&(identical(other.controlPassword, controlPassword) || other.controlPassword == controlPassword)&&(identical(other.critterIndex, critterIndex) || other.critterIndex == critterIndex)&&(identical(other.lastSeenEpochMs, lastSeenEpochMs) || other.lastSeenEpochMs == lastSeenEpochMs)&&const DeepCollectionEquality().equals(other.lastShadeColor, lastShadeColor)&&const DeepCollectionEquality().equals(other.lastBaseColor, lastBaseColor)&&(identical(other.lastKnownIsMesh, lastKnownIsMesh) || other.lastKnownIsMesh == lastKnownIsMesh)&&(identical(other.hasCommitChar, hasCommitChar) || other.hasCommitChar == hasCommitChar));
}

@JsonKey(includeFromJson: false, includeToJson: false)
@override
int get hashCode => Object.hash(runtimeType,id,name,controlPassword,critterIndex,lastSeenEpochMs,const DeepCollectionEquality().hash(lastShadeColor),const DeepCollectionEquality().hash(lastBaseColor),lastKnownIsMesh,hasCommitChar);

@override
String toString() {
  return 'InventoryLamp(id: $id, name: $name, controlPassword: $controlPassword, critterIndex: $critterIndex, lastSeenEpochMs: $lastSeenEpochMs, lastShadeColor: $lastShadeColor, lastBaseColor: $lastBaseColor, lastKnownIsMesh: $lastKnownIsMesh, hasCommitChar: $hasCommitChar)';
}


}

/// @nodoc
abstract mixin class $InventoryLampCopyWith<$Res>  {
  factory $InventoryLampCopyWith(InventoryLamp value, $Res Function(InventoryLamp) _then) = _$InventoryLampCopyWithImpl;
@useResult
$Res call({
 String id, String name, String? controlPassword, int? critterIndex, int? lastSeenEpochMs, List<int>? lastShadeColor, List<int>? lastBaseColor, bool? lastKnownIsMesh, bool? hasCommitChar
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
@pragma('vm:prefer-inline') @override $Res call({Object? id = null,Object? name = null,Object? controlPassword = freezed,Object? critterIndex = freezed,Object? lastSeenEpochMs = freezed,Object? lastShadeColor = freezed,Object? lastBaseColor = freezed,Object? lastKnownIsMesh = freezed,Object? hasCommitChar = freezed,}) {
  return _then(_self.copyWith(
id: null == id ? _self.id : id // ignore: cast_nullable_to_non_nullable
as String,name: null == name ? _self.name : name // ignore: cast_nullable_to_non_nullable
as String,controlPassword: freezed == controlPassword ? _self.controlPassword : controlPassword // ignore: cast_nullable_to_non_nullable
as String?,critterIndex: freezed == critterIndex ? _self.critterIndex : critterIndex // ignore: cast_nullable_to_non_nullable
as int?,lastSeenEpochMs: freezed == lastSeenEpochMs ? _self.lastSeenEpochMs : lastSeenEpochMs // ignore: cast_nullable_to_non_nullable
as int?,lastShadeColor: freezed == lastShadeColor ? _self.lastShadeColor : lastShadeColor // ignore: cast_nullable_to_non_nullable
as List<int>?,lastBaseColor: freezed == lastBaseColor ? _self.lastBaseColor : lastBaseColor // ignore: cast_nullable_to_non_nullable
as List<int>?,lastKnownIsMesh: freezed == lastKnownIsMesh ? _self.lastKnownIsMesh : lastKnownIsMesh // ignore: cast_nullable_to_non_nullable
as bool?,hasCommitChar: freezed == hasCommitChar ? _self.hasCommitChar : hasCommitChar // ignore: cast_nullable_to_non_nullable
as bool?,
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

@optionalTypeArgs TResult maybeWhen<TResult extends Object?>(TResult Function( String id,  String name,  String? controlPassword,  int? critterIndex,  int? lastSeenEpochMs,  List<int>? lastShadeColor,  List<int>? lastBaseColor,  bool? lastKnownIsMesh,  bool? hasCommitChar)?  $default,{required TResult orElse(),}) {final _that = this;
switch (_that) {
case _InventoryLamp() when $default != null:
return $default(_that.id,_that.name,_that.controlPassword,_that.critterIndex,_that.lastSeenEpochMs,_that.lastShadeColor,_that.lastBaseColor,_that.lastKnownIsMesh,_that.hasCommitChar);case _:
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

@optionalTypeArgs TResult when<TResult extends Object?>(TResult Function( String id,  String name,  String? controlPassword,  int? critterIndex,  int? lastSeenEpochMs,  List<int>? lastShadeColor,  List<int>? lastBaseColor,  bool? lastKnownIsMesh,  bool? hasCommitChar)  $default,) {final _that = this;
switch (_that) {
case _InventoryLamp():
return $default(_that.id,_that.name,_that.controlPassword,_that.critterIndex,_that.lastSeenEpochMs,_that.lastShadeColor,_that.lastBaseColor,_that.lastKnownIsMesh,_that.hasCommitChar);case _:
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

@optionalTypeArgs TResult? whenOrNull<TResult extends Object?>(TResult? Function( String id,  String name,  String? controlPassword,  int? critterIndex,  int? lastSeenEpochMs,  List<int>? lastShadeColor,  List<int>? lastBaseColor,  bool? lastKnownIsMesh,  bool? hasCommitChar)?  $default,) {final _that = this;
switch (_that) {
case _InventoryLamp() when $default != null:
return $default(_that.id,_that.name,_that.controlPassword,_that.critterIndex,_that.lastSeenEpochMs,_that.lastShadeColor,_that.lastBaseColor,_that.lastKnownIsMesh,_that.hasCommitChar);case _:
  return null;

}
}

}

/// @nodoc
@JsonSerializable()

class _InventoryLamp implements InventoryLamp {
  const _InventoryLamp({required this.id, required this.name, this.controlPassword, this.critterIndex, this.lastSeenEpochMs, final  List<int>? lastShadeColor, final  List<int>? lastBaseColor, this.lastKnownIsMesh, this.hasCommitChar}): _lastShadeColor = lastShadeColor,_lastBaseColor = lastBaseColor;
  factory _InventoryLamp.fromJson(Map<String, dynamic> json) => _$InventoryLampFromJson(json);

@override final  String id;
@override final  String name;
@override final  String? controlPassword;
/// Persistent random critter pick (1-8) assigned at adopt/add time so
/// each lamp keeps the same critter friend across sessions and across
/// the connecting/preview surfaces. Nullable for legacy entries adopted
/// before this field existed — consumers fall back to a deviceId hash.
@override final  int? critterIndex;
@override final  int? lastSeenEpochMs;
/// Cached last-seen colors written by `controlNotifier._updateSeen`
/// on every successful connect-and-read and every settled slider
/// drag. Persisted via `inventory.v1` in SharedPreferences and read
/// back by `resolveLampColors` to render My Lamps / picker tiles.
///
/// Shape: `[R, G, B, W]` (4 ints). Legacy entries written before
/// this field grew the W byte may be `[R, G, B]` (length 3) — the
/// resolver treats those as `W = 0`, preserving the prior render.
 final  List<int>? _lastShadeColor;
/// Cached last-seen colors written by `controlNotifier._updateSeen`
/// on every successful connect-and-read and every settled slider
/// drag. Persisted via `inventory.v1` in SharedPreferences and read
/// back by `resolveLampColors` to render My Lamps / picker tiles.
///
/// Shape: `[R, G, B, W]` (4 ints). Legacy entries written before
/// this field grew the W byte may be `[R, G, B]` (length 3) — the
/// resolver treats those as `W = 0`, preserving the prior render.
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

/// Last observed `isMesh` (capability bit 1 in the adv) — true when
/// the lamp speaks the app's v0x03 mesh protocol, false for legacy
/// BT-only firmware. Set by `nearby_lamps_notifier` whenever a fresh
/// adv arrives. `lampRouteResolver` reads this when the live roster
/// is empty (lamp out of range) so an offline legacy BT-only lamp
/// routes to BtOnlyLampScreen instead of stranding the user on
/// ConnectingView forever. Nullable for legacy inventory entries
/// written before this field existed — resolvers default to
/// "assume mesh-capable" for those, mirroring the pre-fix behavior.
@override final  bool? lastKnownIsMesh;
/// GATT-discovered presence of CHAR_COMMIT (Phase A firmware).
/// Populated post-connect by `inventory_notifier.updateHasCommitChar`.
/// Null when not yet probed (pre-existing inventory entries, or the
/// mid-connect window before the probe resolves). Read sites MUST
/// default null to false via `?? false` — null means "unknown", and
/// unknown should fall back to the legacy Save-pill flow rather than
/// incorrectly enabling Phase A behavior.
@override final  bool? hasCommitChar;

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
  return identical(this, other) || (other.runtimeType == runtimeType&&other is _InventoryLamp&&(identical(other.id, id) || other.id == id)&&(identical(other.name, name) || other.name == name)&&(identical(other.controlPassword, controlPassword) || other.controlPassword == controlPassword)&&(identical(other.critterIndex, critterIndex) || other.critterIndex == critterIndex)&&(identical(other.lastSeenEpochMs, lastSeenEpochMs) || other.lastSeenEpochMs == lastSeenEpochMs)&&const DeepCollectionEquality().equals(other._lastShadeColor, _lastShadeColor)&&const DeepCollectionEquality().equals(other._lastBaseColor, _lastBaseColor)&&(identical(other.lastKnownIsMesh, lastKnownIsMesh) || other.lastKnownIsMesh == lastKnownIsMesh)&&(identical(other.hasCommitChar, hasCommitChar) || other.hasCommitChar == hasCommitChar));
}

@JsonKey(includeFromJson: false, includeToJson: false)
@override
int get hashCode => Object.hash(runtimeType,id,name,controlPassword,critterIndex,lastSeenEpochMs,const DeepCollectionEquality().hash(_lastShadeColor),const DeepCollectionEquality().hash(_lastBaseColor),lastKnownIsMesh,hasCommitChar);

@override
String toString() {
  return 'InventoryLamp(id: $id, name: $name, controlPassword: $controlPassword, critterIndex: $critterIndex, lastSeenEpochMs: $lastSeenEpochMs, lastShadeColor: $lastShadeColor, lastBaseColor: $lastBaseColor, lastKnownIsMesh: $lastKnownIsMesh, hasCommitChar: $hasCommitChar)';
}


}

/// @nodoc
abstract mixin class _$InventoryLampCopyWith<$Res> implements $InventoryLampCopyWith<$Res> {
  factory _$InventoryLampCopyWith(_InventoryLamp value, $Res Function(_InventoryLamp) _then) = __$InventoryLampCopyWithImpl;
@override @useResult
$Res call({
 String id, String name, String? controlPassword, int? critterIndex, int? lastSeenEpochMs, List<int>? lastShadeColor, List<int>? lastBaseColor, bool? lastKnownIsMesh, bool? hasCommitChar
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
@override @pragma('vm:prefer-inline') $Res call({Object? id = null,Object? name = null,Object? controlPassword = freezed,Object? critterIndex = freezed,Object? lastSeenEpochMs = freezed,Object? lastShadeColor = freezed,Object? lastBaseColor = freezed,Object? lastKnownIsMesh = freezed,Object? hasCommitChar = freezed,}) {
  return _then(_InventoryLamp(
id: null == id ? _self.id : id // ignore: cast_nullable_to_non_nullable
as String,name: null == name ? _self.name : name // ignore: cast_nullable_to_non_nullable
as String,controlPassword: freezed == controlPassword ? _self.controlPassword : controlPassword // ignore: cast_nullable_to_non_nullable
as String?,critterIndex: freezed == critterIndex ? _self.critterIndex : critterIndex // ignore: cast_nullable_to_non_nullable
as int?,lastSeenEpochMs: freezed == lastSeenEpochMs ? _self.lastSeenEpochMs : lastSeenEpochMs // ignore: cast_nullable_to_non_nullable
as int?,lastShadeColor: freezed == lastShadeColor ? _self._lastShadeColor : lastShadeColor // ignore: cast_nullable_to_non_nullable
as List<int>?,lastBaseColor: freezed == lastBaseColor ? _self._lastBaseColor : lastBaseColor // ignore: cast_nullable_to_non_nullable
as List<int>?,lastKnownIsMesh: freezed == lastKnownIsMesh ? _self.lastKnownIsMesh : lastKnownIsMesh // ignore: cast_nullable_to_non_nullable
as bool?,hasCommitChar: freezed == hasCommitChar ? _self.hasCommitChar : hasCommitChar // ignore: cast_nullable_to_non_nullable
as bool?,
  ));
}


}

// dart format on
