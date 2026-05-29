// lib/features/onboarding/presentation/widgets/add_lamp_wifi_step.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../application/add_lamp_notifier.dart';
import '../../domain/add_lamp_state.dart';

class AddLampWifiStep extends ConsumerStatefulWidget {
  const AddLampWifiStep({super.key});

  @override
  ConsumerState<AddLampWifiStep> createState() => _AddLampWifiStepState();
}

class _AddLampWifiStepState extends ConsumerState<AddLampWifiStep> {
  late final TextEditingController _ssid =
      TextEditingController(text: ref.read(addLampNotifierProvider).ssid);
  late final TextEditingController _pwd =
      TextEditingController(text: ref.read(addLampNotifierProvider).password);

  @override
  void dispose() {
    _ssid.dispose();
    _pwd.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final notifier = ref.read(addLampNotifierProvider.notifier);
    final state = ref.watch(addLampNotifierProvider);
    final canContinue = state.ssid.isNotEmpty && state.password.isNotEmpty;
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'Home WiFi',
            style: TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 18,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            'The lamp connects to your home WiFi so other lamps can find it.',
            style: TextStyle(color: BrandColors.fogGrey),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _ssid,
            autofocus: true,
            onChanged: notifier.setSsid,
            decoration: const InputDecoration(
              labelText: 'Network name (SSID)',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _pwd,
            obscureText: true,
            onChanged: notifier.setPassword,
            decoration: const InputDecoration(
              labelText: 'Password',
              border: OutlineInputBorder(),
            ),
          ),
          const Spacer(),
          if (state.status == AddLampStatus.error)
            Padding(
              padding: const EdgeInsets.only(bottom: 12),
              child: Text(
                state.errorMessage ?? 'Setup failed',
                style: const TextStyle(color: Colors.redAccent),
              ),
            ),
          Row(
            children: [
              TextButton(
                onPressed: state.status == AddLampStatus.working
                    ? null
                    : notifier.previous,
                child: const Text('Back'),
              ),
              const Spacer(),
              FilledButton(
                onPressed: (canContinue &&
                        state.status != AddLampStatus.working)
                    ? notifier.submit
                    : null,
                child: state.status == AddLampStatus.working
                    ? const SizedBox(
                        width: 16,
                        height: 16,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Text('Set up lamp'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
