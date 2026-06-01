import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/ble/ble_client.dart';
import '../../../core/ble/ble_client_provider.dart';
import '../../../core/ble/uuids.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/info_panel.dart';
import '../../control/application/control_notifier.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../application/wifi_notifier.dart';
import '../domain/wifi_state.dart';

/// Home Mode pane — presence-only detection of a home WiFi network.
///
/// The lamp never associates to the AP. It periodically scans for nearby
/// SSIDs (background scans gated to BT-disconnected windows so they don't
/// stress the shared radio) and treats the user's selected SSID being
/// visible as "I'm at home". No password is stored or transmitted, which
/// also closes off the spoofed-AP password-capture attack.
///
/// While the user is on this page, the firmware unconditionally treats
/// home mode as ACTIVE via the CHAR_HOME_MODE_FOCUS signal — so the
/// brightness slider previews the home brightness in real time. When
/// the user leaves the page, the focus signal clears and the firmware
/// falls back to "configurator" mode (home mode off) for the rest of
/// the BT session.
class HomeModeScreen extends ConsumerStatefulWidget {
  const HomeModeScreen({super.key, required this.lampId});
  final String lampId;

  @override
  ConsumerState<HomeModeScreen> createState() => _HomeModeScreenState();
}

class _HomeModeScreenState extends ConsumerState<HomeModeScreen> {
  bool _didKickoffScan = false;

  // Captured in initState so dispose can fire the focus-off write WITHOUT
  // touching `ref` after super.dispose() — the previous pattern of
  // ref.read(...) in dispose was tripping
  // `_lifecycleState != _ElementLifecycle.defunct` framework asserts
  // because the Element is mid-teardown by then.
  late BleClient _ble;

  @override
  void initState() {
    super.initState();
    _ble = ref.read(bleClientProvider);
    // Tell the lamp "user is on the Home Mode page": forces home-mode
    // gate ON so the brightness slider previews home brightness live,
    // and routes incoming CHAR_BRIGHTNESS writes to homeMode.brightness.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      _writeFocus(true);
    });
  }

  @override
  void dispose() {
    // Tell the lamp "user is leaving" — fire-and-forget via the captured
    // BleClient. The firmware also clears the flag on BT disconnect as
    // a safety net, so a missed exit (app killed mid-page) is recovered.
    _writeFocus(false);
    super.dispose();
  }

  void _writeFocus(bool active) {
    unawaited(_ble.write(
      widget.lampId,
      BleUuids.controlService,
      BleUuids.homeModeFocus,
      Uint8List.fromList([active ? 1 : 0]),
    ).catchError((_) {
      // best-effort — BT may already be torn down by route pop
    }));
  }

  Future<void> _selectSsid(String ssid) async {
    // 1. Tell the firmware to persist the new home SSID (CHAR_WIFI_OP
    //    setHomeSsid → persistConfigToNvs firmware-side).
    await ref.read(wifiNotifierProvider(widget.lampId).notifier)
        .setHomeSsid(ssid);
    // 2. Mirror into controlNotifier state so the UI reflects it
    //    immediately (CHAR_HOME_SECTION has no NOTIFY subscription).
    if (!mounted) return;
    ref.read(controlNotifierProvider(widget.lampId).notifier)
        .setHomeSsid(ssid);
  }

  void _onForget() {
    // Firmware clears + persists; mirror to app state.
    ref.read(wifiNotifierProvider(widget.lampId).notifier).forget();
    final cn = ref.read(controlNotifierProvider(widget.lampId).notifier);
    cn.setHomeSsid('');
  }

  @override
  Widget build(BuildContext context) {
    final controlAsync = ref.watch(controlNotifierProvider(widget.lampId));
    final wifiAsync = ref.watch(wifiNotifierProvider(widget.lampId));
    final wifiNotifier =
        ref.read(wifiNotifierProvider(widget.lampId).notifier);

    return Scaffold(
      appBar: AppBar(
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () => GoRouter.maybeOf(context)?.pop(),
        ),
        title: const Text('Home Mode'),
      ),
      body: controlAsync.when(
        loading: () => ConnectingView(deviceId: widget.lampId),
        error: (e, _) => Center(
          child: Text('Could not reach this lamp: $e',
              style: const TextStyle(color: BrandColors.fogGrey)),
        ),
        data: (state) {
          final notifier =
              ref.read(controlNotifierProvider(widget.lampId).notifier);
          final wifi = wifiAsync.value ?? const WifiState();

          final hasSaved = state.home.ssid.isNotEmpty;
          final isScanning = wifi.state == 'scanning';

          // Kick off a scan on first paint when no SSID is saved (we need
          // a network list for the user to pick from). When a network IS
          // saved we don't show the picker, so no scan needed.
          if (!_didKickoffScan && !hasSaved && wifi.scanResults.isEmpty &&
              wifi.state != 'scanning') {
            _didKickoffScan = true;
            WidgetsBinding.instance.addPostFrameCallback((_) {
              wifiNotifier.scan();
            });
          }

          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              const InfoPanel(
                child: Text(
                  'Home Mode quiets the lamp for everyday use — when the '
                  'lamp sees your home Wi-Fi nearby it pauses social '
                  'greetings and switches to a calmer brightness. The '
                  "lamp doesn't connect to the network or store its "
                  'password — it just listens for the name in the air.',
                ),
              ),
              const SizedBox(height: 16),

              if (hasSaved) ...[
                _ConnectionStatusRow(
                  label: 'Home network: ${state.home.ssid}',
                  onForget: _onForget,
                ),
                const SizedBox(height: 12),
              ] else ...[
                Row(
                  children: [
                    const Expanded(
                      child: Text(
                        'Pick your home network',
                        style: TextStyle(
                          color: BrandColors.fogGrey,
                          fontWeight: FontWeight.w600,
                          fontSize: 13,
                          letterSpacing: 0.5,
                        ),
                      ),
                    ),
                    SizedBox(
                      width: 36,
                      height: 36,
                      child: isScanning
                          ? const Padding(
                              padding: EdgeInsets.all(8),
                              child:
                                  CircularProgressIndicator(strokeWidth: 2),
                            )
                          : IconButton(
                              icon: const Icon(Icons.refresh, size: 20),
                              tooltip: 'Scan for networks',
                              onPressed:
                                  isScanning ? null : wifiNotifier.scan,
                              padding: EdgeInsets.zero,
                            ),
                    ),
                  ],
                ),
                const SizedBox(height: 4),
                if (wifi.scanResults.isEmpty && !isScanning)
                  const Padding(
                    padding: EdgeInsets.symmetric(vertical: 16),
                    child: Text(
                      'Tap refresh to scan for networks.',
                      style: TextStyle(color: BrandColors.fogGrey),
                      textAlign: TextAlign.center,
                    ),
                  ),
                ...wifi.scanResults.map((r) => ListTile(
                      contentPadding:
                          const EdgeInsets.symmetric(horizontal: 0),
                      leading: _RssiBars(rssi: r.rssi),
                      title: Text(r.ssid),
                      trailing: r.encrypted
                          ? const Icon(Icons.lock_outline,
                              size: 16, color: BrandColors.slateGrey)
                          : null,
                      onTap: () => _selectSsid(r.ssid),
                    )),
              ],

              // Home brightness — slider always available; the firmware
              // routes CHAR_BRIGHTNESS to home.brightness while this page
              // is open (via CHAR_HOME_MODE_FOCUS), so dragging the
              // slider previews the home brightness live on the lamp.
              const SizedBox(height: 24),
              const Text(
                'Home Mode brightness',
                style: TextStyle(
                  color: BrandColors.headerYellow,
                  fontSize: 13,
                  fontWeight: FontWeight.w700,
                  letterSpacing: 0.8,
                ),
              ),
              Row(
                children: [
                  Expanded(
                    child: Slider(
                      value: state.home.brightness.toDouble(),
                      min: 0,
                      max: 100,
                      divisions: 100,
                      onChanged: (v) =>
                          notifier.setHomeBrightness(v.round()),
                    ),
                  ),
                  SizedBox(
                    width: 48,
                    child: Text(
                      '${state.home.brightness}%',
                      textAlign: TextAlign.right,
                      style: const TextStyle(
                        color: BrandColors.fogGrey,
                        fontSize: 12,
                        fontFamily: 'monospace',
                      ),
                    ),
                  ),
                ],
              ),
            ],
          );
        },
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Private widgets
// ---------------------------------------------------------------------------

class _ConnectionStatusRow extends StatelessWidget {
  const _ConnectionStatusRow({
    required this.label,
    required this.onForget,
  });
  final String label;
  final VoidCallback onForget;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(
          child: Text(label,
              style: const TextStyle(color: BrandColors.lampWhite)),
        ),
        TextButton.icon(
          icon: const Icon(Icons.wifi_off, size: 16),
          label: const Text('Forget network'),
          onPressed: onForget,
        ),
      ],
    );
  }
}

class _RssiBars extends StatelessWidget {
  const _RssiBars({required this.rssi});
  final int rssi;

  int get _bars => rssi >= -55
      ? 4
      : rssi >= -65
          ? 3
          : rssi >= -75
              ? 2
              : 1;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 22,
      height: 22,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.end,
        mainAxisSize: MainAxisSize.min,
        children: List.generate(4, (i) {
          final filled = i < _bars;
          return Container(
            width: 3,
            height: 6.0 + i * 4,
            margin: const EdgeInsets.symmetric(horizontal: 1),
            decoration: BoxDecoration(
              color: filled
                  ? BrandColors.lumenGreen
                  : BrandColors.slateGrey.withValues(alpha: 0.35),
              borderRadius: BorderRadius.circular(1),
            ),
          );
        }),
      ),
    );
  }
}
