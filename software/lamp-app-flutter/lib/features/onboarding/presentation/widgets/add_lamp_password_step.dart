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

Future<void> _confirmSkip(BuildContext context, AddLampNotifier notifier) async {
  final ok = await showDialog<bool>(
    context: context,
    builder: (ctx) => AlertDialog(
      backgroundColor: BrandColors.midnightBlack,
      title: const Text('Adopt without a password?',
          style: TextStyle(color: BrandColors.lampWhite)),
      content: const Text(
        'Not recommended. Anyone within Bluetooth range will be able to '
        'control this lamp. You can set a password later from the lamp\'s '
        'Setup tab.',
        style: TextStyle(color: BrandColors.fogGrey),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(ctx).pop(false),
          child: const Text('Cancel'),
        ),
        FilledButton(
          style: FilledButton.styleFrom(
              backgroundColor: BrandColors.slateGrey),
          onPressed: () => Navigator.of(ctx).pop(true),
          child: const Text('Skip anyway'),
        ),
      ],
    ),
  );
  if (ok != true) return;
  notifier.setPassword('');
  await notifier.submit();
}

class _AddLampPasswordStepState extends ConsumerState<AddLampPasswordStep> {
  late final TextEditingController _pwd = TextEditingController(
    text: ref.read(addLampNotifierProvider).password,
  );
  final _confirm = TextEditingController();

  @override
  void dispose() {
    _pwd.dispose();
    _confirm.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final notifier = ref.read(addLampNotifierProvider.notifier);
    final state = ref.watch(addLampNotifierProvider);
    final showMismatch =
        _confirm.text.isNotEmpty && _confirm.text != state.password;
    final canContinue = state.password.isNotEmpty &&
        _confirm.text == state.password;
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
          if (state.error == AddLampError.wrongPassword) ...[
            const SizedBox(height: 8),
            const Text(
              "Wrong password. The lamp didn't accept it — try again.",
              style: TextStyle(color: BrandColors.error),
              textAlign: TextAlign.center,
            ),
          ],
          const SizedBox(height: 16),
          TextField(
            controller: _pwd,
            autofocus: true,
            obscureText: true,
            onChanged: (v) {
              notifier.setPassword(v);
              setState(() {}); // re-evaluate the mismatch banner + button
            },
            decoration: const InputDecoration(
              labelText: 'Lamp password',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _confirm,
            obscureText: true,
            onChanged: (_) => setState(() {}),
            decoration: InputDecoration(
              labelText: 'Confirm password',
              border: const OutlineInputBorder(),
              errorText: showMismatch ? "Doesn't match" : null,
            ),
          ),
          const Spacer(),
          if (state.status == AddLampStatus.error &&
              state.error != AddLampError.wrongPassword)
            Padding(
              padding: const EdgeInsets.only(bottom: 12),
              child: Text(
                state.errorMessage ?? 'Setup failed',
                style: const TextStyle(color: BrandColors.error),
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
              TextButton(
                onPressed: (state.status != AddLampStatus.working &&
                        state.step != AddLampStep.verifying)
                    ? () => _confirmSkip(context, notifier)
                    : null,
                style: TextButton.styleFrom(
                    foregroundColor: BrandColors.slateGrey),
                child: const Text('Skip'),
              ),
              const SizedBox(width: 8),
              FilledButton(
                onPressed: (canContinue &&
                        state.status != AddLampStatus.working &&
                        state.step != AddLampStep.verifying)
                    ? notifier.submit
                    : null,
                child: (state.status == AddLampStatus.working ||
                        state.step == AddLampStep.verifying)
                    ? const Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          SizedBox(
                            width: 16,
                            height: 16,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          ),
                          SizedBox(width: 8),
                          Text('Verifying…'),
                        ],
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
