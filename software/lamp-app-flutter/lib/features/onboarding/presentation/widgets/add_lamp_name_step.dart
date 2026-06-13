// lib/features/onboarding/presentation/widgets/add_lamp_name_step.dart
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../application/add_lamp_notifier.dart';

const _suggestions = ['jacko', 'melonie', 'bowery'];

class AddLampNameStep extends ConsumerStatefulWidget {
  const AddLampNameStep({super.key});

  @override
  ConsumerState<AddLampNameStep> createState() => _AddLampNameStepState();
}

class _AddLampNameStepState extends ConsumerState<AddLampNameStep> {
  late final TextEditingController _controller =
      TextEditingController(text: ref.read(addLampNotifierProvider).name);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final notifier = ref.read(addLampNotifierProvider.notifier);
    final state = ref.watch(addLampNotifierProvider);
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'Name this lamp',
            style: TextStyle(
              color: BrandColors.lampWhite,
              fontSize: 18,
              fontWeight: FontWeight.w600,
            ),
          ),
          const SizedBox(height: 16),
          TextField(
            controller: _controller,
            autofocus: true,
            onChanged: notifier.setName,
            decoration: const InputDecoration(
              labelText: 'Lamp name',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 16),
          Wrap(
            spacing: 8,
            children: _suggestions.map((s) {
              return ActionChip(
                label: Text(s),
                onPressed: () {
                  _controller.text = s;
                  notifier.setName(s);
                },
              );
            }).toList(),
          ),
          const Spacer(),
          Row(
            children: [
              TextButton(
                onPressed: notifier.previous,
                child: const Text('Back'),
              ),
              const Spacer(),
              FilledButton(
                onPressed: state.name.isEmpty ? null : notifier.next,
                child: const Text('Continue'),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
