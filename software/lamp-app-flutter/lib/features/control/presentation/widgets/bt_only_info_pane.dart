import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../../core/routing/routes.dart';
import '../../../../core/widgets/info_panel.dart';
import '../../../nearby/application/nearby_lamps_notifier.dart';

/// Renders a "this lamp is BT-only — set up home Wi-Fi" callout on the
/// Control screen when the active lamp's advertisement reports
/// `onMesh: false`, or when the lamp is not in the nearby list at all
/// (direct-BLE-only case). Hidden when the lamp is currently on the mesh.
class BtOnlyInfoPane extends ConsumerWidget {
  const BtOnlyInfoPane({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final onMesh = ref.watch(nearbyLampsNotifierProvider.select(
        (list) => list.firstWhereOrNull((l) => l.id == lampId)?.onMesh));
    if (onMesh == true) return const SizedBox.shrink();
    return InfoPanel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'This lamp is reachable on Bluetooth only. Set up Home Wi-Fi '
            'so you can use it from anywhere in your home.',
          ),
          const SizedBox(height: 8),
          Align(
            alignment: Alignment.centerLeft,
            child: TextButton.icon(
              icon: const Icon(Icons.wifi, size: 16),
              label: const Text('Set up Home Wi-Fi'),
              onPressed: () => context.push(AppRoutes.homeWifi(lampId)),
            ),
          ),
        ],
      ),
    );
  }
}
