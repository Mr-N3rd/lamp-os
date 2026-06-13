import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../../../core/widgets/status_dot.dart';
import '../../../nearby/application/nearby_lamps_notifier.dart';
import '../../../nearby/domain/nearby_lamp.dart';
import '../../application/add_lamp_notifier.dart';

class AddLampScanStep extends ConsumerWidget {
  const AddLampScanStep({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final lamps = ref.watch(nearbyLampsNotifierProvider);
    if (lamps.isEmpty) {
      return const Center(
        child: Text(
          'Scanning for lamps...',
          textAlign: TextAlign.center,
          style: TextStyle(color: BrandColors.fogGrey),
        ),
      );
    }
    return ListView.separated(
      padding: const EdgeInsets.all(16),
      itemCount: lamps.length,
      separatorBuilder: (_, index) => const SizedBox(height: 8),
      itemBuilder: (context, i) => _LampRow(lamp: lamps[i]),
    );
  }
}

class _LampRow extends ConsumerWidget {
  const _LampRow({required this.lamp});
  final NearbyLamp lamp;

  Future<void> _onTap(BuildContext context, WidgetRef ref) async {
    if (lamp.isFactoryDefault) {
      await ref.read(addLampNotifierProvider.notifier).select(lamp.id);
    } else {
      // No confirm dialog — `add()` sets state.step to `done` and the
      // AddLampShell will swap in the AddLampDoneStep ("X is ready"),
      // which serves as the visual confirmation.
      await ref
          .read(addLampNotifierProvider.notifier)
          .add(deviceId: lamp.id, name: lamp.name);
    }
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    return InkWell(
      borderRadius: BorderRadius.circular(12),
      onTap: () => _onTap(context, ref),
      child: Container(
        padding: const EdgeInsets.all(12),
        decoration: BoxDecoration(
          color: Colors.white.withValues(alpha: 0.04),
          borderRadius: BorderRadius.circular(12),
          border:
              Border.all(color: Colors.white.withValues(alpha: 0.06)),
        ),
        child: Row(
          children: [
            // BLE adv tells us bluetooth-reachable; v2-firmware lamps also
            // carry a mesh-state byte (`NearbyLamp.onMesh`). Light bright
            // green when the lamp is on the mesh, otherwise faded blue.
            StatusDot(
              kind: lamp.onMesh
                  ? StatusKind.mesh
                  : StatusKind.bluetooth,
              size: 14,
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    lamp.name.isEmpty ? '(unnamed)' : lamp.name,
                    style: const TextStyle(
                      color: BrandColors.lampWhite,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  Text(
                    '${lamp.id} · ${lamp.rssi} dBm',
                    style: const TextStyle(
                      color: BrandColors.slateGrey,
                      fontSize: 11,
                    ),
                  ),
                ],
              ),
            ),
            _Pill(factoryDefault: lamp.isFactoryDefault),
          ],
        ),
      ),
    );
  }
}

class _Pill extends StatelessWidget {
  const _Pill({required this.factoryDefault});
  final bool factoryDefault;

  @override
  Widget build(BuildContext context) {
    final base =
        factoryDefault ? BrandColors.amberGold : BrandColors.lumenGreen;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(999),
        color: base.withValues(alpha: 0.18),
      ),
      child: Text(
        factoryDefault ? 'adopt' : 'add',
        style: TextStyle(
          fontSize: 10,
          color: base,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }
}

