// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'nearby_lamps_notifier.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning

@ProviderFor(bleScanner)
final bleScannerProvider = BleScannerProvider._();

final class BleScannerProvider
    extends $FunctionalProvider<BleScanner, BleScanner, BleScanner>
    with $Provider<BleScanner> {
  BleScannerProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'bleScannerProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$bleScannerHash();

  @$internal
  @override
  $ProviderElement<BleScanner> $createElement($ProviderPointer pointer) =>
      $ProviderElement(pointer);

  @override
  BleScanner create(Ref ref) {
    return bleScanner(ref);
  }

  /// {@macro riverpod.override_with_value}
  Override overrideWithValue(BleScanner value) {
    return $ProviderOverride(
      origin: this,
      providerOverride: $SyncValueProvider<BleScanner>(value),
    );
  }
}

String _$bleScannerHash() => r'a07f000755b01d92c7803d7975aa70b450011579';

@ProviderFor(NearbyLampsNotifier)
final nearbyLampsNotifierProvider = NearbyLampsNotifierProvider._();

final class NearbyLampsNotifierProvider
    extends $NotifierProvider<NearbyLampsNotifier, List<NearbyLamp>> {
  NearbyLampsNotifierProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'nearbyLampsNotifierProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$nearbyLampsNotifierHash();

  @$internal
  @override
  NearbyLampsNotifier create() => NearbyLampsNotifier();

  /// {@macro riverpod.override_with_value}
  Override overrideWithValue(List<NearbyLamp> value) {
    return $ProviderOverride(
      origin: this,
      providerOverride: $SyncValueProvider<List<NearbyLamp>>(value),
    );
  }
}

String _$nearbyLampsNotifierHash() =>
    r'fab8bb03dc63ba63b9779991ffdc271dabd56dfe';

abstract class _$NearbyLampsNotifier extends $Notifier<List<NearbyLamp>> {
  List<NearbyLamp> build();
  @$mustCallSuper
  @override
  void runBuild() {
    final ref = this.ref as $Ref<List<NearbyLamp>, List<NearbyLamp>>;
    final element =
        ref.element
            as $ClassProviderElement<
              AnyNotifier<List<NearbyLamp>, List<NearbyLamp>>,
              List<NearbyLamp>,
              Object?,
              Object?
            >;
    element.handleCreate(ref, build);
  }
}
