import '../../../core/routing/routes.dart';
import '../domain/nearby_lamp.dart';

/// Pick the right route for opening [lampId] based on what the BLE
/// scanner currently sees. A lamp whose adv reports
/// `isMesh: false` (legacy v1 firmware or any build pre-mesh-protocol)
/// can't complete the full GATT control handshake, so it goes to the
/// dedicated [AppRoutes.btOnly] info pane instead of [AppRoutes.control]
/// where the user would just be stuck on the ConnectingView.
///
/// Offline lamps (no nearby hit at all) route to control: that's where
/// the ConnectingView lives, and it's also where the lamp will land
/// once it comes back into range — the user sees "Connecting…" briefly
/// rather than a separate offline screen.
///
/// Centralizing this so every entry point — MyLamps tile,
/// AppBar picker sheet, post-adoption done step — uses the same logic.
/// Hardcoded `AppRoutes.control` calls in those flows previously sent
/// BT-only lamps to a connect screen they could never satisfy.
String routeForLamp(String lampId, List<NearbyLamp> nearby) {
  for (final n in nearby) {
    if (n.id == lampId) {
      return n.isMesh ? AppRoutes.control(lampId) : AppRoutes.btOnly(lampId);
    }
  }
  // Not in range right now — fall through to control + ConnectingView.
  return AppRoutes.control(lampId);
}
