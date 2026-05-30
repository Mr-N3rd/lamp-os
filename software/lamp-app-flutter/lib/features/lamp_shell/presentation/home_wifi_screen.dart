import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/info_panel.dart';
import '../../control/application/control_notifier.dart';
import '../../control/presentation/widgets/connecting_view.dart';

/// Home Wi-Fi configuration pane.
///
/// Phase 1 (this commit): keeps the existing SSID + password text-field
/// flow that the Setup screen already had, just moved into its own
/// drill-down screen so the parent Setup tab can be a tidy row list.
///
/// Phase 2 (next chunk): replace the manual SSID field with a live scan
/// powered by the firmware's `wifiOp` / `wifiState` characteristics so
/// the user can tap a network from a list and enter a password — that's
/// what the old Vue app's `HomeNetworkSheet.vue` did.
class HomeWifiScreen extends ConsumerStatefulWidget {
  const HomeWifiScreen({super.key, required this.lampId});
  final String lampId;

  @override
  ConsumerState<HomeWifiScreen> createState() => _HomeWifiScreenState();
}

class _HomeWifiScreenState extends ConsumerState<HomeWifiScreen> {
  final _ssidCtrl = TextEditingController();
  final _passwordCtrl = TextEditingController();
  bool _seededSsid = false;

  @override
  void dispose() {
    _ssidCtrl.dispose();
    _passwordCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final async = ref.watch(controlNotifierProvider(widget.lampId));
    return Scaffold(
      appBar: AppBar(
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () => GoRouter.maybeOf(context)?.pop(),
        ),
        title: const Text('Home Wi-Fi'),
      ),
      body: async.when(
        loading: () => ConnectingView(deviceId: widget.lampId),
        error: (e, _) => Center(
          child: Text('Could not reach this lamp: $e',
              style: const TextStyle(color: BrandColors.fogGrey)),
        ),
        data: (state) {
          if (!_seededSsid) {
            _ssidCtrl.text = state.home.ssid;
            _seededSsid = true;
          }
          final notifier =
              ref.read(controlNotifierProvider(widget.lampId).notifier);
          final hasExisting = state.home.password == '********' ||
              state.home.password.isNotEmpty;
          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              const InfoPanel(
                child: Text(
                  'Join your lamp to your home Wi-Fi so it can participate in '
                  'the mesh, accept smart-home control, and receive '
                  'over-the-air updates. Live network scanning is coming '
                  'soon — enter the SSID and password manually for now.',
                ),
              ),
              const SizedBox(height: 16),
              TextField(
                controller: _ssidCtrl,
                decoration: const InputDecoration(labelText: 'Network (SSID)'),
                style: const TextStyle(color: BrandColors.lampWhite),
                onChanged: notifier.setHomeSsid,
              ),
              const SizedBox(height: 12),
              TextField(
                controller: _passwordCtrl,
                obscureText: true,
                decoration: InputDecoration(
                  labelText: 'Password',
                  hintText:
                      hasExisting ? '(unchanged — type to replace)' : null,
                  suffixIcon: hasExisting && _passwordCtrl.text.isEmpty
                      ? const Padding(
                          padding: EdgeInsets.only(right: 8),
                          child: Tooltip(
                            message:
                                'A password is set; leaving this blank keeps it.',
                            child: Icon(Icons.lock,
                                size: 16,
                                color: BrandColors.lumenGreen),
                          ),
                        )
                      : null,
                ),
                style: const TextStyle(color: BrandColors.lampWhite),
                onChanged: (v) {
                  setState(() {});
                  notifier
                      .setHomePassword(v.isEmpty ? '********' : v);
                },
              ),
              if (state.home.ssid.isNotEmpty) ...[
                const SizedBox(height: 24),
                TextButton.icon(
                  icon: const Icon(Icons.delete_outline, size: 18),
                  style: TextButton.styleFrom(
                      foregroundColor: BrandColors.error),
                  label: const Text('Forget this network'),
                  onPressed: () {
                    notifier.setHomeSsid('');
                    notifier.setHomePassword('');
                    _ssidCtrl.clear();
                    _passwordCtrl.clear();
                  },
                ),
              ],
            ],
          );
        },
      ),
    );
  }
}
