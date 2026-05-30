import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/info_panel.dart';
import '../../control/application/control_notifier.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../application/wifi_notifier.dart';
import '../domain/wifi_state.dart';

/// Home Wi-Fi configuration pane.
///
/// Powered by [WifiNotifier] — shows live scan results from the lamp's
/// `wifiState` characteristic, lets the user tap a network to enter a
/// password, and shows a connected / forget-network row when already joined.
class HomeWifiScreen extends ConsumerStatefulWidget {
  const HomeWifiScreen({super.key, required this.lampId});
  final String lampId;

  @override
  ConsumerState<HomeWifiScreen> createState() => _HomeWifiScreenState();
}

class _HomeWifiScreenState extends ConsumerState<HomeWifiScreen> {
  bool _didKickoff = false;

  @override
  void initState() {
    super.initState();
    // Listen for connection failures and surface them as SnackBars.
    // Must be registered after first build — do it here via addPostFrameCallback
    // to ensure the context has a Scaffold.
  }

  String _errorMessage(String? lastError) {
    switch (lastError) {
      case 'auth':
        return 'Wrong password';
      case 'noap':
        return 'Network not found';
      case 'timeout':
        return 'Connection timed out';
      default:
        return 'Connection failed';
    }
  }

  Future<void> _promptPassword(BuildContext context, WifiScanResult r) async {
    final ctrl = TextEditingController();
    await showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      backgroundColor: BrandColors.midnightBlack,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
      ),
      builder: (ctx) => Padding(
        padding: EdgeInsets.only(
          bottom: MediaQuery.of(ctx).viewInsets.bottom + 16,
          top: 16,
          left: 16,
          right: 16,
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(
              'Join "${r.ssid}"',
              style: const TextStyle(
                color: BrandColors.lampWhite,
                fontWeight: FontWeight.w600,
                fontSize: 16,
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: ctrl,
              obscureText: r.encrypted,
              autofocus: true,
              decoration: InputDecoration(
                labelText: r.encrypted ? 'Password' : 'Password (none required)',
              ),
            ),
            const SizedBox(height: 12),
            Align(
              alignment: Alignment.centerRight,
              child: FilledButton(
                onPressed: () {
                  final pwd = ctrl.text;
                  Navigator.of(ctx).pop();
                  ref
                      .read(wifiNotifierProvider(widget.lampId).notifier)
                      .connect(r.ssid, pwd);
                },
                child: const Text('Connect'),
              ),
            ),
          ],
        ),
      ),
    );
    ctrl.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final wifiAsync = ref.watch(wifiNotifierProvider(widget.lampId));
    final notifier = ref.read(wifiNotifierProvider(widget.lampId).notifier);

    // Listen for failed state and show a SnackBar.
    ref.listen(wifiNotifierProvider(widget.lampId), (prev, next) {
      if (prev?.value?.state != 'failed' && next.value?.state == 'failed') {
        final msg = _errorMessage(next.value?.lastError);
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(msg)),
        );
      }
    });

    return Scaffold(
      appBar: AppBar(
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () => GoRouter.maybeOf(context)?.pop(),
        ),
        title: const Text('Home Wi-Fi'),
      ),
      body: wifiAsync.when(
        loading: () => ConnectingView(deviceId: widget.lampId),
        error: (e, _) => Center(
          child: Text(
            'Could not reach this lamp: $e',
            style: const TextStyle(color: BrandColors.fogGrey),
          ),
        ),
        data: (wifi) {
          // Post-frame scan kickoff on first render with empty + idle state.
          if (!_didKickoff && wifi.scanResults.isEmpty && wifi.state == 'idle') {
            _didKickoff = true;
            WidgetsBinding.instance.addPostFrameCallback((_) {
              notifier.scan();
            });
          }

          // Read the saved SSID from the control state for the "saved but not
          // connected" row.
          final savedSsid = ref
              .watch(controlNotifierProvider(widget.lampId)
                  .select((async) => async.value?.home.ssid ?? ''));

          final isConnected = wifi.state == 'connected';
          final hasSaved = savedSsid.isNotEmpty;
          final isScanning = wifi.state == 'scanning';

          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              const InfoPanel(
                child: Text(
                  'Join your lamp to your home Wi-Fi so it can participate in '
                  'the mesh, accept smart-home control, and receive '
                  'over-the-air updates. Tap a network to join it.',
                ),
              ),
              const SizedBox(height: 16),

              // Connected row.
              if (isConnected) ...[
                _ConnectionStatusRow(
                  label: 'Connected to ${wifi.ssid} (${wifi.ip})',
                  onForget: notifier.forget,
                ),
                const SizedBox(height: 8),
              ] else if (hasSaved) ...[
                // Saved but not currently connected.
                _ConnectionStatusRow(
                  label: 'Saved: $savedSsid',
                  onForget: notifier.forget,
                ),
                const SizedBox(height: 8),
              ],

              // "Networks" header row with refresh button.
              Row(
                children: [
                  const Expanded(
                    child: Text(
                      'Networks',
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
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : IconButton(
                            icon: const Icon(Icons.refresh, size: 20),
                            tooltip: 'Scan for networks',
                            onPressed: isScanning ? null : notifier.scan,
                            padding: EdgeInsets.zero,
                          ),
                  ),
                ],
              ),
              const SizedBox(height: 4),

              // Empty-state hint.
              if (wifi.scanResults.isEmpty && !isScanning)
                const Padding(
                  padding: EdgeInsets.symmetric(vertical: 16),
                  child: Text(
                    'Tap refresh to scan for networks.',
                    style: TextStyle(color: BrandColors.fogGrey),
                    textAlign: TextAlign.center,
                  ),
                ),

              // Scan results.
              ...wifi.scanResults.map((r) {
                final inFlight = r.ssid == wifi.ssid &&
                    wifi.state == 'connecting';
                return ListTile(
                  contentPadding: const EdgeInsets.symmetric(horizontal: 0),
                  leading: _RssiBars(rssi: r.rssi),
                  title: Text(r.ssid),
                  trailing: r.encrypted
                      ? const Icon(
                          Icons.lock_outline,
                          size: 16,
                          color: BrandColors.slateGrey,
                        )
                      : null,
                  onTap: inFlight ? null : () => _promptPassword(context, r),
                );
              }),
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
          child: Text(
            label,
            style: const TextStyle(color: BrandColors.lampWhite),
          ),
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
