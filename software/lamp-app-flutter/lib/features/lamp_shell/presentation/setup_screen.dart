import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../../control/application/control_notifier.dart';
import '../../control/application/control_state.dart';
import '../../control/domain/sections.dart';
import '../../control/presentation/widgets/connecting_view.dart';

class SetupScreen extends ConsumerWidget {
  const SetupScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final async = ref.watch(controlNotifierProvider(lampId));
    return async.when(
      loading: () => ConnectingView(deviceId: lampId),
      error: (e, _) => Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Text(
            'Could not reach this lamp: $e',
            style: const TextStyle(color: BrandColors.fogGrey),
            textAlign: TextAlign.center,
          ),
        ),
      ),
      data: (state) => _SetupBody(lampId: lampId, state: state),
    );
  }
}

class _SetupBody extends ConsumerWidget {
  const _SetupBody({required this.lampId, required this.state});
  final String lampId;
  final ControlState state;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final n = ref.read(controlNotifierProvider(lampId).notifier);
    return ListView(
      padding: const EdgeInsets.symmetric(vertical: 8),
      children: [
        _Section(
          title: 'Lamp name',
          child: _NameField(initial: state.lamp.name, onChanged: n.setLampName),
        ),
        _Section(
          title: 'Home WiFi',
          child: _HomeFields(home: state.home, notifier: n),
        ),
        if (state.home.ssid.isNotEmpty)
          _CollapsibleSection(
            title: 'Smart Home (MQTT)',
            initiallyExpanded: state.mqtt.enabled,
            child: _MqttFields(mqtt: state.mqtt, notifier: n),
          ),
        if (state.lamp.advancedEnabled)
          _CollapsibleSection(
            title: 'Advanced',
            initiallyExpanded: true,
            child: _AdvancedFields(state: state, notifier: n),
          )
        else
          _EnableAdvancedTile(
            onEnable: () => n.setLampAdvancedEnabled(true),
          ),
      ],
    );
  }
}

class _Section extends StatelessWidget {
  const _Section({required this.title, required this.child});
  final String title;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.04),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            title,
            style: const TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 14,
              fontWeight: FontWeight.w600,
              letterSpacing: 0.4,
            ),
          ),
          const SizedBox(height: 12),
          child,
        ],
      ),
    );
  }
}

class _CollapsibleSection extends StatelessWidget {
  const _CollapsibleSection({
    required this.title,
    required this.child,
    this.initiallyExpanded = false,
  });
  final String title;
  final Widget child;
  final bool initiallyExpanded;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      decoration: BoxDecoration(
        color: Colors.white.withValues(alpha: 0.04),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
      ),
      child: Theme(
        data: Theme.of(context).copyWith(dividerColor: Colors.transparent),
        child: ExpansionTile(
          shape: const RoundedRectangleBorder(side: BorderSide.none),
          initiallyExpanded: initiallyExpanded,
          title: Text(
            title,
            style: const TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 14,
              fontWeight: FontWeight.w600,
              letterSpacing: 0.4,
            ),
          ),
          childrenPadding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
          children: [child],
        ),
      ),
    );
  }
}

class _NameField extends StatefulWidget {
  const _NameField({required this.initial, required this.onChanged});
  final String initial;
  final ValueChanged<String> onChanged;

  @override
  State<_NameField> createState() => _NameFieldState();
}

class _NameFieldState extends State<_NameField> {
  late final TextEditingController _ctrl =
      TextEditingController(text: widget.initial);

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return TextField(
      controller: _ctrl,
      style: const TextStyle(color: BrandColors.lampWhite),
      decoration: const InputDecoration(
        labelText: 'Lamp name',
        border: OutlineInputBorder(),
      ),
      onChanged: widget.onChanged,
    );
  }
}

class _HomeFields extends StatefulWidget {
  const _HomeFields({required this.home, required this.notifier});
  final HomeSection home;
  final ControlNotifier notifier;

  @override
  State<_HomeFields> createState() => _HomeFieldsState();
}

class _HomeFieldsState extends State<_HomeFields> {
  late final TextEditingController _ssid =
      TextEditingController(text: widget.home.ssid);
  late final TextEditingController _password = TextEditingController();
  late final TextEditingController _brightness =
      TextEditingController(text: '${widget.home.brightness}');

  bool get _hasExistingPassword =>
      widget.home.password == '********' || widget.home.password.isNotEmpty;

  @override
  void dispose() {
    _ssid.dispose();
    _password.dispose();
    _brightness.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        TextField(
          controller: _ssid,
          style: const TextStyle(color: BrandColors.lampWhite),
          decoration: const InputDecoration(
            labelText: 'SSID',
            border: OutlineInputBorder(),
          ),
          onChanged: widget.notifier.setHomeSsid,
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _password,
          obscureText: true,
          style: const TextStyle(color: BrandColors.lampWhite),
          decoration: InputDecoration(
            labelText: 'Password',
            hintText: _hasExistingPassword
                ? '(unchanged — type to replace)'
                : null,
            border: const OutlineInputBorder(),
          ),
          onChanged: (v) {
            // Empty input → keep the sentinel; the save merge will strip it.
            widget.notifier.setHomePassword(v.isEmpty ? '********' : v);
          },
        ),
        const SizedBox(height: 12),
        Row(
          children: [
            const Text(
              'Brightness',
              style: TextStyle(color: BrandColors.lampWhite, fontSize: 13),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Slider(
                min: 0,
                max: 100,
                divisions: 100,
                value: widget.home.brightness.toDouble(),
                onChanged: (v) => widget.notifier.setHomeBrightness(v.round()),
              ),
            ),
            SizedBox(
              width: 40,
              child: Text(
                '${widget.home.brightness}%',
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
  }
}

class _MqttFields extends StatefulWidget {
  const _MqttFields({required this.mqtt, required this.notifier});
  final MqttSection mqtt;
  final ControlNotifier notifier;

  @override
  State<_MqttFields> createState() => _MqttFieldsState();
}

class _MqttFieldsState extends State<_MqttFields> {
  late final _host = TextEditingController(text: widget.mqtt.brokerHost);
  late final _port = TextEditingController(text: '${widget.mqtt.brokerPort}');
  late final _user = TextEditingController(text: widget.mqtt.username);
  late final _password = TextEditingController();
  late final _topic = TextEditingController(text: widget.mqtt.topicPrefix);

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
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text('Enabled',
              style: TextStyle(color: BrandColors.lampWhite, fontSize: 13)),
          value: widget.mqtt.enabled,
          onChanged: widget.notifier.setMqttEnabled,
        ),
        const SizedBox(height: 4),
        TextField(
          controller: _host,
          style: const TextStyle(color: BrandColors.lampWhite),
          decoration: const InputDecoration(
            labelText: 'Broker host',
            border: OutlineInputBorder(),
          ),
          onChanged: widget.notifier.setMqttBrokerHost,
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _port,
          keyboardType: TextInputType.number,
          style: const TextStyle(color: BrandColors.lampWhite),
          decoration: const InputDecoration(
            labelText: 'Broker port',
            border: OutlineInputBorder(),
          ),
          onChanged: (v) {
            final n = int.tryParse(v);
            if (n != null) widget.notifier.setMqttBrokerPort(n);
          },
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _user,
          style: const TextStyle(color: BrandColors.lampWhite),
          decoration: const InputDecoration(
            labelText: 'Username',
            border: OutlineInputBorder(),
          ),
          onChanged: widget.notifier.setMqttUsername,
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _password,
          obscureText: true,
          style: const TextStyle(color: BrandColors.lampWhite),
          decoration: InputDecoration(
            labelText: 'Password',
            hintText: (widget.mqtt.password == '********' ||
                    widget.mqtt.password.isNotEmpty)
                ? '(unchanged — type to replace)'
                : null,
            border: const OutlineInputBorder(),
          ),
          onChanged: (v) {
            widget.notifier.setMqttPassword(v.isEmpty ? '********' : v);
          },
        ),
        const SizedBox(height: 12),
        TextField(
          controller: _topic,
          style: const TextStyle(color: BrandColors.lampWhite),
          decoration: const InputDecoration(
            labelText: 'Topic prefix',
            border: OutlineInputBorder(),
          ),
          onChanged: widget.notifier.setMqttTopicPrefix,
        ),
      ],
    );
  }
}

class _AdvancedFields extends StatelessWidget {
  const _AdvancedFields({required this.state, required this.notifier});
  final ControlState state;
  final ControlNotifier notifier;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Text('Base strip type',
            style: TextStyle(color: BrandColors.lampWhite, fontSize: 13)),
        const SizedBox(height: 4),
        SegmentedButton<int>(
          showSelectedIcon: false,
          segments: const [
            ButtonSegment(value: 4, label: Text('RGBW (4 bpp)')),
            ButtonSegment(value: 3, label: Text('RGB (3 bpp)')),
          ],
          selected: {state.base.bpp},
          onSelectionChanged: (s) => notifier.setBaseBpp(s.first),
        ),
        const SizedBox(height: 12),
        const Text('Shade strip type',
            style: TextStyle(color: BrandColors.lampWhite, fontSize: 13)),
        const SizedBox(height: 4),
        SegmentedButton<int>(
          showSelectedIcon: false,
          segments: const [
            ButtonSegment(value: 4, label: Text('RGBW (4 bpp)')),
            ButtonSegment(value: 3, label: Text('RGB (3 bpp)')),
          ],
          selected: {state.shade.bpp},
          onSelectionChanged: (s) => notifier.setShadeBpp(s.first),
        ),
        const SizedBox(height: 12),
        SwitchListTile(
          contentPadding: EdgeInsets.zero,
          title: const Text(
            'Advanced enabled',
            style: TextStyle(color: BrandColors.lampWhite, fontSize: 13),
          ),
          value: state.lamp.advancedEnabled,
          onChanged: notifier.setLampAdvancedEnabled,
        ),
      ],
    );
  }
}

class _EnableAdvancedTile extends StatelessWidget {
  const _EnableAdvancedTile({required this.onEnable});
  final VoidCallback onEnable;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
      child: TextButton(
        onPressed: onEnable,
        child: const Text('Enable advanced settings'),
      ),
    );
  }
}
