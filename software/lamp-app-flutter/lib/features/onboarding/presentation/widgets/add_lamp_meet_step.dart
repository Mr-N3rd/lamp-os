import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_svg/flutter_svg.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../../control/presentation/widgets/critter_asset.dart';
import '../../application/add_lamp_notifier.dart';

/// Sits between Name and Password. Once the user has given the lamp a name
/// (emotional buy-in is highest here), introduce the headline features in
/// the adoption-pet voice so they know what they're bringing home.
class AddLampMeetStep extends ConsumerWidget {
  const AddLampMeetStep({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final state = ref.watch(addLampNotifierProvider);
    final notifier = ref.read(addLampNotifierProvider.notifier);
    // critterIndex isn't picked until submit() persists the lamp, so fall
    // back to the deviceId-hash variant — the resolver in critter_asset
    // does that for us when index is null.
    final critter = critterAssetFor(
      critterIndex: null,
      deviceId: state.deviceId,
    );
    final name = state.name.isEmpty ? 'your lamp' : state.name;
    return Padding(
      padding: const EdgeInsets.all(24),
      child: SizedBox(
        width: double.infinity,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
            SvgPicture.asset(critter, width: 96, height: 96),
            const SizedBox(height: 12),
            Text(
              'Meet $name',
              textAlign: TextAlign.center,
              style: const TextStyle(
                color: BrandColors.lampWhite,
                fontSize: 20,
                fontWeight: FontWeight.w600,
              ),
            ),
            const SizedBox(height: 6),
            const Text(
              "Here's what they get up to once they settle in.",
              textAlign: TextAlign.center,
              style: TextStyle(color: BrandColors.fogGrey, fontSize: 13),
            ),
            const SizedBox(height: 20),
            const Expanded(
              child: SingleChildScrollView(
                child: Column(
                  children: [
                    _Feature(
                      icon: Icons.brightness_6,
                      title: 'Two-tone body',
                      text:
                          "Their base and shade glow on their own — paint moods you couldn't with one strip.",
                    ),
                    _Feature(
                      icon: Icons.auto_awesome,
                      title: 'Four moods',
                      text:
                          'Glitch, Pulse, Breath, Shift — layer them up so your lamp shimmers without you lifting a finger.',
                    ),
                    _Feature(
                      icon: Icons.hub,
                      title: 'Mesh friends',
                      text:
                          "Lamps notice each other over the air and greet, mimic, follow. Adopt a few and they'll keep each other company.",
                    ),
                    _Feature(
                      icon: Icons.home,
                      title: 'Home sense',
                      text:
                          'They feel when your home Wi-Fi is nearby and quiet down on their own — no timers to fuss with.',
                    ),
                  ],
                ),
              ),
            ),
            Row(
              children: [
                TextButton(
                  onPressed: notifier.previous,
                  child: const Text('Back'),
                ),
                const Spacer(),
                TextButton(
                  onPressed: notifier.next,
                  style: TextButton.styleFrom(
                      foregroundColor: BrandColors.slateGrey),
                  child: const Text('Skip the tour'),
                ),
                const SizedBox(width: 8),
                FilledButton(
                  onPressed: notifier.next,
                  child: const Text('Sounds good'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _Feature extends StatelessWidget {
  const _Feature({required this.icon, required this.title, required this.text});
  final IconData icon;
  final String title;
  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 14),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 36,
            height: 36,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: BrandColors.glowPink.withValues(alpha: 0.16),
            ),
            child: Icon(icon, color: BrandColors.glowPink, size: 20),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: const TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  text,
                  style: const TextStyle(
                    color: BrandColors.fogGrey,
                    fontSize: 12,
                    height: 1.4,
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
