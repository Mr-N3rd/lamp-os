// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'seen_lamps_notifier.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning
/// Persistent record of every lamp we've ever heard via BLE adv. Mirrors
/// the same scanner stream `nearbyLampsNotifier` listens to, but keeps
/// entries forever (until evicted by the size cap) so the Info page can
/// show a "Seen before" list alongside the live "Nearby" one.
///
/// State is loaded synchronously from SharedPreferences on first build()
/// and persisted with [_persistDebounce] coalescing on every adv update.

@ProviderFor(SeenLampsNotifier)
final seenLampsNotifierProvider = SeenLampsNotifierProvider._();

/// Persistent record of every lamp we've ever heard via BLE adv. Mirrors
/// the same scanner stream `nearbyLampsNotifier` listens to, but keeps
/// entries forever (until evicted by the size cap) so the Info page can
/// show a "Seen before" list alongside the live "Nearby" one.
///
/// State is loaded synchronously from SharedPreferences on first build()
/// and persisted with [_persistDebounce] coalescing on every adv update.
final class SeenLampsNotifierProvider
    extends $NotifierProvider<SeenLampsNotifier, List<SeenLamp>> {
  /// Persistent record of every lamp we've ever heard via BLE adv. Mirrors
  /// the same scanner stream `nearbyLampsNotifier` listens to, but keeps
  /// entries forever (until evicted by the size cap) so the Info page can
  /// show a "Seen before" list alongside the live "Nearby" one.
  ///
  /// State is loaded synchronously from SharedPreferences on first build()
  /// and persisted with [_persistDebounce] coalescing on every adv update.
  SeenLampsNotifierProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'seenLampsNotifierProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$seenLampsNotifierHash();

  @$internal
  @override
  SeenLampsNotifier create() => SeenLampsNotifier();

  /// {@macro riverpod.override_with_value}
  Override overrideWithValue(List<SeenLamp> value) {
    return $ProviderOverride(
      origin: this,
      providerOverride: $SyncValueProvider<List<SeenLamp>>(value),
    );
  }
}

String _$seenLampsNotifierHash() => r'7ca249745506bdd32694b21cf6871599d789a52b';

/// Persistent record of every lamp we've ever heard via BLE adv. Mirrors
/// the same scanner stream `nearbyLampsNotifier` listens to, but keeps
/// entries forever (until evicted by the size cap) so the Info page can
/// show a "Seen before" list alongside the live "Nearby" one.
///
/// State is loaded synchronously from SharedPreferences on first build()
/// and persisted with [_persistDebounce] coalescing on every adv update.

abstract class _$SeenLampsNotifier extends $Notifier<List<SeenLamp>> {
  List<SeenLamp> build();
  @$mustCallSuper
  @override
  void runBuild() {
    final ref = this.ref as $Ref<List<SeenLamp>, List<SeenLamp>>;
    final element =
        ref.element
            as $ClassProviderElement<
              AnyNotifier<List<SeenLamp>, List<SeenLamp>>,
              List<SeenLamp>,
              Object?,
              Object?
            >;
    element.handleCreate(ref, build);
  }
}
