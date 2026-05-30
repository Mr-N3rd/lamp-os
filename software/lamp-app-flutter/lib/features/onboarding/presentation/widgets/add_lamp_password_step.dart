import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../application/add_lamp_notifier.dart';
import '../../domain/add_lamp_state.dart';

class AddLampPasswordStep extends ConsumerStatefulWidget {
  const AddLampPasswordStep({super.key});

  @override
  ConsumerState<AddLampPasswordStep> createState() =>
      _AddLampPasswordStepState();
}

class _AddLampPasswordStepState extends ConsumerState<AddLampPasswordStep> {
  late final TextEditingController _pwd = TextEditingController(
    text: ref.read(addLampNotifierProvider).password,
  );

  @override
  void dispose() {
    _pwd.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final notifier = ref.read(addLampNotifierProvider.notifier);
    final state = ref.watch(addLampNotifierProvider);
    final canContinue = state.password.isNotEmpty;
    return Padding(
      padding: const EdgeInsets.all(24),
      // SizedBox.expand fills the Padding's width so `crossAxisAlignment
      // .center` lands the heading at screen-center (a bare Column shrinks
      // to its widest child and pins to the left edge of the Padding).
      child: SizedBox(
        width: double.infinity,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
          const Text(
            'Set a password',
            textAlign: TextAlign.center,
            style: TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 18,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 8),
          const Text(
            'This password protects the lamp so only phones you trust can '
            'control it.',
            textAlign: TextAlign.center,
            style: TextStyle(color: BrandColors.fogGrey),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _pwd,
            autofocus: true,
            obscureText: true,
            onChanged: notifier.setPassword,
            decoration: const InputDecoration(
              labelText: 'Lamp password',
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
                onPressed:
                    (canContinue && state.status != AddLampStatus.working)
                        ? notifier.submit
                        : null,
                child: state.status == AddLampStatus.working
                    ? const SizedBox(
                        width: 16,
                        height: 16,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Text('Adopt lamp'),
              ),
            ],
          ),
          ],
        ),
      ),
    );
  }
}
