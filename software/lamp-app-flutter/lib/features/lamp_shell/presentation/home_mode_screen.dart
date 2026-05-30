import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/info_panel.dart';
import '../../control/application/control_notifier.dart';
import '../../control/presentation/widgets/connecting_view.dart';

/// Home Mode pane — what the lamp does once it's on home Wi-Fi:
///   - Home-mode brightness (separate from the regular brightness).
///   - Smart-home (MQTT) hook-up.
///
/// Both controls are gated on the lamp having an SSID configured;
/// otherwise we show an empty-state pointing the user at the Home Wi-Fi
/// screen first.
class HomeModeScreen extends ConsumerStatefulWidget {
  const HomeModeScreen({super.key, required this.lampId});
  final String lampId;

  @override
  ConsumerState<HomeModeScreen> createState() => _HomeModeScreenState();
}

class _HomeModeScreenState extends ConsumerState<HomeModeScreen> {
  final _host = TextEditingController();
  final _port = TextEditingController();
  final _user = TextEditingController();
  final _password = TextEditingController();
  final _topic = TextEditingController();
  bool _seeded = false;

  @override
  void dispose() {
    _host.dispose();
    _port.dispose();
    _user.dispose();
    _password.dispose();
    _topic.dispose();
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
        title: const Text('Home Mode'),
      ),
      body: async.when(
        loading: () => ConnectingView(deviceId: widget.lampId),
        error: (e, _) => Center(
          child: Text('Could not reach this lamp: $e',
              style: const TextStyle(color: BrandColors.fogGrey)),
        ),
        data: (state) {
          final notifier =
              ref.read(controlNotifierProvider(widget.lampId).notifier);
          if (state.home.ssid.isEmpty) {
            return const Padding(
              padding: EdgeInsets.all(24),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Icon(Icons.wifi_off,
                      color: BrandColors.fogGrey, size: 48),
                  SizedBox(height: 16),
                  Text(
                    'Set up Home Wi-Fi first.',
                    textAlign: TextAlign.center,
                    style: TextStyle(
                      color: BrandColors.lampWhite,
                      fontSize: 16,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  SizedBox(height: 8),
                  Text(
                    'Home Mode brightness and smart-home control take effect '
                    'once your lamp is on the home Wi-Fi network.',
                    textAlign: TextAlign.center,
                    style: TextStyle(
                        color: BrandColors.fogGrey, fontSize: 13),
                  ),
                ],
              ),
            );
          }
          if (!_seeded) {
            _host.text = state.mqtt.brokerHost;
            _port.text = '${state.mqtt.brokerPort}';
            _user.text = state.mqtt.username;
            _topic.text = state.mqtt.topicPrefix;
            _seeded = true;
          }
          final mqttHasPwd = state.mqtt.password == '********' ||
              state.mqtt.password.isNotEmpty;
          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              const InfoPanel(
                child: Text(
                  'When the lamp is on your home Wi-Fi it uses a separate '
                  'brightness level — useful for ambient lighting at home.',
                ),
              ),
              const SizedBox(height: 16),
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
                title: const Text(
                  'Enable MQTT',
                  style: TextStyle(
                      color: BrandColors.lampWhite, fontSize: 14),
                ),
                value: state.mqtt.enabled,
                onChanged: notifier.setMqttEnabled,
              ),
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
                decoration: const InputDecoration(labelText: 'Username'),
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
          );
        },
      ),
    );
  }
}
