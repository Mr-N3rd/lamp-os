import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/info_panel.dart';
import '../../control/application/control_notifier.dart';
import '../../control/presentation/widgets/connecting_view.dart';
import '../application/wifi_notifier.dart';
import '../domain/wifi_state.dart';

/// Home Mode pane — everything the lamp does once it's joined to the
/// user's home Wi-Fi:
///   - Pick a Wi-Fi network + join it.
///   - Home-mode brightness (separate from the regular brightness).
///   - Smart-home (MQTT) hook-up.
///
/// "Home Mode" and "Home Wi-Fi" used to be separate Setup rows pointing at
/// separate screens — they're a single concept (the lamp is on the home
/// network and behaving as a home-mode device), so they were merged into
/// this one pane.
///
/// When the user successfully joins a network the screen auto-pops back to
/// Setup after a short delay so the connection feedback is visible without
/// trapping the user on the scan list.
class HomeModeScreen extends ConsumerStatefulWidget {
  const HomeModeScreen({super.key, required this.lampId});
  final String lampId;

  @override
  ConsumerState<HomeModeScreen> createState() => _HomeModeScreenState();
}

class _HomeModeScreenState extends ConsumerState<HomeModeScreen> {
  bool _didKickoffScan = false;
  bool _autoPoppedAfterConnect = false;
  // Only auto-pop on connects the user kicked off from this page. Without
  // this gate the `ref.listen` block below fires on its first registration
  // (where prev == null and next.value.state == 'connected') and pops the
  // user out immediately — which now happens on every entry once the page
  // brings Wi-Fi up via enterHomePreview.
  bool _userInitiatedConnect = false;
  final _host = TextEditingController();
  final _port = TextEditingController();
  final _user = TextEditingController();
  final _password = TextEditingController();
  final _topic = TextEditingController();
  bool _seededMqtt = false;

  @override
  void initState() {
    super.initState();
    // Tell the firmware to switch into home mode (bring up Wi-Fi STA with
    // saved creds) while we're on this page. Credentials are known-good —
    // the user can only reach this page after a successful Wi-Fi setup.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      ref
          .read(controlNotifierProvider(widget.lampId).notifier)
          .enterHomePreview();
    });
  }

  @override
  void dispose() {
    // Tell the firmware to drop Wi-Fi again so the rest of the BT session
    // runs without coexistence pressure. Fire-and-forget: the firmware
    // also cleans up on BT disconnect, so a missed exit (e.g. user killed
    // the app) is recovered automatically.
    ref
        .read(controlNotifierProvider(widget.lampId).notifier)
        .exitHomePreview();
    _host.dispose();
    _port.dispose();
    _user.dispose();
    _password.dispose();
    _topic.dispose();
    super.dispose();
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
    await showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      backgroundColor: BrandColors.midnightBlack,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
      ),
      builder: (ctx) => _WifiPasswordSheet(
        scanResult: r,
        onConnect: (pwd) {
          Navigator.of(ctx).pop();
          _userInitiatedConnect = true;
          ref
              .read(wifiNotifierProvider(widget.lampId).notifier)
              .connect(r.ssid, pwd);
        },
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final controlAsync = ref.watch(controlNotifierProvider(widget.lampId));
    final wifiAsync = ref.watch(wifiNotifierProvider(widget.lampId));
    final wifiNotifier =
        ref.read(wifiNotifierProvider(widget.lampId).notifier);

    // SnackBar on connect failure.
    ref.listen(wifiNotifierProvider(widget.lampId), (prev, next) {
      if (prev?.value?.state != 'failed' && next.value?.state == 'failed') {
        final msg = _errorMessage(next.value?.lastError);
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg)));
        }
      }
      // Auto-pop back to Setup once we've actually joined a network — gives
      // the user the success state for ~1.2s, then drops them on the Setup
      // tab which shows Home Mode as enabled with the saved SSID. Gated on
      // _userInitiatedConnect so an already-connected lamp (or the new
      // enterHomePreview-driven connect) doesn't bounce the user out on
      // page entry.
      final justConnected = _userInitiatedConnect &&
          prev?.value?.state != 'connected' &&
          next.value?.state == 'connected';
      if (justConnected && !_autoPoppedAfterConnect) {
        _autoPoppedAfterConnect = true;
        // Capture the router before the await so we don't re-use the
        // BuildContext across the async gap.
        final router = GoRouter.maybeOf(context);
        Future<void>.delayed(const Duration(milliseconds: 1200), () {
          if (!mounted) return;
          router?.pop();
        });
      }
    });

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

          // Kick off a scan on first paint when nothing's queued.
          if (!_didKickoffScan &&
              wifi.scanResults.isEmpty &&
              wifi.state == 'idle') {
            _didKickoffScan = true;
            WidgetsBinding.instance.addPostFrameCallback((_) {
              wifiNotifier.scan();
            });
          }

          // Seed MQTT controllers the first time we land on a hydrated state.
          if (!_seededMqtt) {
            _host.text = state.mqtt.brokerHost;
            _port.text = '${state.mqtt.brokerPort}';
            _user.text = state.mqtt.username;
            _topic.text = state.mqtt.topicPrefix;
            _seededMqtt = true;
          }

          final isConnected = wifi.state == 'connected';
          final hasSaved = state.home.ssid.isNotEmpty;
          final isScanning = wifi.state == 'scanning';
          final isConnecting = wifi.state == 'connecting';
          final mqttHasPwd = state.mqtt.password == '********' ||
              state.mqtt.password.isNotEmpty;

          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              const InfoPanel(
                child: Text(
                  'Join your lamp to your home Wi-Fi so it can participate '
                  'in the mesh, use a separate home brightness, and accept '
                  'smart-home control via MQTT.',
                ),
              ),
              const SizedBox(height: 16),

              // ────────────────────────────────────────────────────────
              // Wi-Fi connection
              // ────────────────────────────────────────────────────────
              if (isConnected) ...[
                _ConnectionStatusRow(
                  label: 'Connected to ${wifi.ssid}'
                      '${wifi.ip != null && wifi.ip!.isNotEmpty
                          ? ' (${wifi.ip})'
                          : ''}',
                  onForget: wifiNotifier.forget,
                ),
                const SizedBox(height: 12),
              ] else if (hasSaved) ...[
                _ConnectionStatusRow(
                  label: 'Saved: ${state.home.ssid}',
                  onForget: wifiNotifier.forget,
                ),
                const SizedBox(height: 12),
              ],

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
                            child:
                                CircularProgressIndicator(strokeWidth: 2),
                          )
                        : IconButton(
                            icon: const Icon(Icons.refresh, size: 20),
                            tooltip: 'Scan for networks',
                            onPressed: isScanning ? null : wifiNotifier.scan,
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

              ...wifi.scanResults.map((r) {
                final inFlight = r.ssid == wifi.ssid && isConnecting;
                return ListTile(
                  contentPadding: const EdgeInsets.symmetric(horizontal: 0),
                  leading: _RssiBars(rssi: r.rssi),
                  title: Text(r.ssid),
                  trailing: r.encrypted
                      ? const Icon(Icons.lock_outline,
                          size: 16, color: BrandColors.slateGrey)
                      : null,
                  onTap: inFlight ? null : () => _promptPassword(context, r),
                );
              }),

              // ────────────────────────────────────────────────────────
              // Home-mode brightness + MQTT (only meaningful once joined)
              // ────────────────────────────────────────────────────────
              if (hasSaved || isConnected) ...[
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
                        onChanged: (v) {
                          final i = v.round();
                          notifier.setHomeBrightness(i);
                          // Live BLE write to CHAR_HOME_PREVIEW. The
                          // firmware has Wi-Fi STA up (from the page's
                          // initState enterHomePreview call), so the lamp
                          // is using home brightness — this updates the
                          // in-memory value, no NVS write until Save.
                          notifier.previewHomeBrightness(i);
                        },
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
                const SizedBox(height: 24),
                const Text(
                  'Smart home (Home Assistant)',
                  style: TextStyle(
                    color: BrandColors.headerYellow,
                    fontSize: 13,
                    fontWeight: FontWeight.w700,
                    letterSpacing: 0.8,
                  ),
                ),
                const SizedBox(height: 8),
                SwitchListTile(
                  contentPadding: EdgeInsets.zero,
                  title: const Text('Enable MQTT',
                      style: TextStyle(
                          color: BrandColors.lampWhite, fontSize: 14)),
                  value: state.mqtt.enabled,
                  onChanged: notifier.setMqttEnabled,
                ),
                if (state.mqtt.enabled) ...[
                  const SizedBox(height: 4),
                  TextField(
                    controller: _host,
                    decoration:
                        const InputDecoration(labelText: 'Broker host'),
                    style: const TextStyle(color: BrandColors.lampWhite),
                    onChanged: notifier.setMqttBrokerHost,
                  ),
                  const SizedBox(height: 12),
                  TextField(
                    controller: _port,
                    keyboardType: TextInputType.number,
                    decoration:
                        const InputDecoration(labelText: 'Broker port'),
                    style: const TextStyle(color: BrandColors.lampWhite),
                    onChanged: (v) {
                      final n = int.tryParse(v);
                      if (n != null) notifier.setMqttBrokerPort(n);
                    },
                  ),
                  const SizedBox(height: 12),
                  TextField(
                    controller: _user,
                    decoration:
                        const InputDecoration(labelText: 'Username'),
                    style: const TextStyle(color: BrandColors.lampWhite),
                    onChanged: notifier.setMqttUsername,
                  ),
                  const SizedBox(height: 12),
                  TextField(
                    controller: _password,
                    obscureText: true,
                    decoration: InputDecoration(
                      labelText: 'Password',
                      hintText: mqttHasPwd
                          ? '(unchanged — type to replace)'
                          : null,
                      suffixIcon: mqttHasPwd && _password.text.isEmpty
                          ? const Padding(
                              padding: EdgeInsets.only(right: 8),
                              child: Tooltip(
                                message:
                                    'A password is set; leaving this blank keeps it.',
                                child: Icon(Icons.lock,
                                    size: 16, color: BrandColors.lumenGreen),
                              ),
                            )
                          : null,
                    ),
                    style: const TextStyle(color: BrandColors.lampWhite),
                    onChanged: (v) {
                      setState(() {});
                      notifier
                          .setMqttPassword(v.isEmpty ? '********' : v);
                    },
                  ),
                  const SizedBox(height: 12),
                  TextField(
                    controller: _topic,
                    decoration:
                        const InputDecoration(labelText: 'Topic prefix'),
                    style: const TextStyle(color: BrandColors.lampWhite),
                    onChanged: notifier.setMqttTopicPrefix,
                  ),
                ],
              ],
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

/// Password-entry body for the join-network modal sheet. Owns its own
/// TextEditingController via State, so the controller lives exactly as
/// long as the sheet's widget subtree — including during the dismiss
/// animation. (Previous bug: creating the controller in the parent and
/// disposing after `await showModalBottomSheet` returned crashed during
/// the dismiss transition.)
class _WifiPasswordSheet extends StatefulWidget {
  const _WifiPasswordSheet({
    required this.scanResult,
    required this.onConnect,
  });
  final WifiScanResult scanResult;
  final void Function(String password) onConnect;

  @override
  State<_WifiPasswordSheet> createState() => _WifiPasswordSheetState();
}

class _WifiPasswordSheetState extends State<_WifiPasswordSheet> {
  final _ctrl = TextEditingController();

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final r = widget.scanResult;
    return Padding(
      padding: EdgeInsets.only(
        bottom: MediaQuery.of(context).viewInsets.bottom + 16,
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
            controller: _ctrl,
            obscureText: r.encrypted,
            autofocus: true,
            decoration: InputDecoration(
              labelText:
                  r.encrypted ? 'Password' : 'Password (none required)',
            ),
          ),
          const SizedBox(height: 12),
          Align(
            alignment: Alignment.centerRight,
            child: FilledButton(
              onPressed: () => widget.onConnect(_ctrl.text),
              child: const Text('Connect'),
            ),
          ),
        ],
      ),
    );
  }
}
