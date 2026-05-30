import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../../core/theme/brand_colors.dart';
import '../../../../core/widgets/info_panel.dart';
import '../../../nearby/application/nearby_lamps_notifier.dart';

/// Surfaces when the active lamp's advertisement reports `onMesh: false`
/// (or the lamp isn't in the nearby list at all). BT-only means the
/// firmware predates ESP-NOW mesh support — fixing it is a firmware
/// update, not a Wi-Fi config tweak. In the meantime the lamp's own
/// access-point + web UI is still available for direct configuration.
class BtOnlyInfoPane extends ConsumerWidget {
  const BtOnlyInfoPane({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final onMesh = ref.watch(nearbyLampsNotifierProvider.select(
        (list) => list.firstWhereOrNull((l) => l.id == lampId)?.onMesh));
    if (onMesh == true) return const SizedBox.shrink();
    return const InfoPanel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            'This lamp is on Bluetooth only — it can\'t join the mesh '
            'on its current firmware.',
          ),
          SizedBox(height: 8),
          Text(
            'To enable mesh: visit update.lamplit.ca on your phone and '
            'follow the firmware-update instructions.',
          ),
          SizedBox(height: 8),
          Text(
            'In the meantime you can still configure this lamp directly: '
            'connect to its Wi-Fi access point and open the web UI.',
            style: TextStyle(color: BrandColors.fogGrey, fontSize: 11),
          ),
        ],
      ),
    );
  }
}
