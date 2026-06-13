// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'firmware_notifier.dart';

// **************************************************************************
// RiverpodGenerator
// **************************************************************************

// GENERATED CODE - DO NOT MODIFY BY HAND
// ignore_for_file: type=lint, type=warning

@ProviderFor(firmwareReleaseClient)
final firmwareReleaseClientProvider = FirmwareReleaseClientProvider._();

final class FirmwareReleaseClientProvider
    extends
        $FunctionalProvider<
          FirmwareReleaseClient,
          FirmwareReleaseClient,
          FirmwareReleaseClient
        >
    with $Provider<FirmwareReleaseClient> {
  FirmwareReleaseClientProvider._()
    : super(
        from: null,
        argument: null,
        retry: null,
        name: r'firmwareReleaseClientProvider',
        isAutoDispose: false,
        dependencies: null,
        $allTransitiveDependencies: null,
      );

  @override
  String debugGetCreateSourceHash() => _$firmwareReleaseClientHash();

  @$internal
  @override
  $ProviderElement<FirmwareReleaseClient> $createElement(
    $ProviderPointer pointer,
  ) => $ProviderElement(pointer);

  @override
  FirmwareReleaseClient create(Ref ref) {
    return firmwareReleaseClient(ref);
  }

  /// {@macro riverpod.override_with_value}
  Override overrideWithValue(FirmwareReleaseClient value) {
    return $ProviderOverride(
      origin: this,
      providerOverride: $SyncValueProvider<FirmwareReleaseClient>(value),
    );
  }
}

String _$firmwareReleaseClientHash() =>
    r'26a4b339c6058f0e58bcdc62751b6b0cc31c4e4e';

@ProviderFor(FirmwareNotifier)
final firmwareNotifierProvider = FirmwareNotifierFamily._();

final class FirmwareNotifierProvider
    extends $NotifierProvider<FirmwareNotifier, FirmwareState> {
  FirmwareNotifierProvider._({
    required FirmwareNotifierFamily super.from,
    required String super.argument,
  }) : super(
         retry: null,
         name: r'firmwareNotifierProvider',
         isAutoDispose: true,
         dependencies: null,
         $allTransitiveDependencies: null,
       );

  @override
  String debugGetCreateSourceHash() => _$firmwareNotifierHash();

  @override
  String toString() {
    return r'firmwareNotifierProvider'
        ''
        '($argument)';
  }

  @$internal
  @override
  FirmwareNotifier create() => FirmwareNotifier();

  /// {@macro riverpod.override_with_value}
  Override overrideWithValue(FirmwareState value) {
    return $ProviderOverride(
      origin: this,
      providerOverride: $SyncValueProvider<FirmwareState>(value),
    );
  }

  @override
  bool operator ==(Object other) {
    return other is FirmwareNotifierProvider && other.argument == argument;
  }

  @override
  int get hashCode {
    return argument.hashCode;
  }
}

String _$firmwareNotifierHash() => r'f7815d481039ccb0d90ffb0d0688020f844a037a';

final class FirmwareNotifierFamily extends $Family
    with
        $ClassFamilyOverride<
          FirmwareNotifier,
          FirmwareState,
          FirmwareState,
          FirmwareState,
          String
        > {
  FirmwareNotifierFamily._()
    : super(
        retry: null,
        name: r'firmwareNotifierProvider',
        dependencies: null,
        $allTransitiveDependencies: null,
        isAutoDispose: true,
      );

  FirmwareNotifierProvider call(String deviceId) =>
      FirmwareNotifierProvider._(argument: deviceId, from: this);

  @override
  String toString() => r'firmwareNotifierProvider';
}

abstract class _$FirmwareNotifier extends $Notifier<FirmwareState> {
  late final _$args = ref.$arg as String;
  String get deviceId => _$args;

  FirmwareState build(String deviceId);
  @$mustCallSuper
  @override
  void runBuild() {
    final ref = this.ref as $Ref<FirmwareState, FirmwareState>;
    final element =
        ref.element
            as $ClassProviderElement<
              AnyNotifier<FirmwareState, FirmwareState>,
              FirmwareState,
              Object?,
              Object?
            >;
    element.handleCreate(ref, () => build(_$args));
  }
}
