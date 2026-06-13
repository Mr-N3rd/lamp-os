import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../../core/routing/routes.dart';
import '../../../../core/theme/brand_colors.dart';
import '../../../../core/widgets/lamp_icon.dart';
import '../../../../core/widgets/status_dot.dart';
import '../../../control/application/control_notifier.dart';
import '../../../inventory/application/active_lamp_notifier.dart';
import '../../../inventory/application/inventory_notifier.dart';
import '../../../inventory/domain/inventory_lamp.dart';
import '../../../lamp_shell/application/lamp_status.dart';
import '../../../nearby/application/nearby_lamps_notifier.dart';
import '../../../nearby/domain/nearby_lamp.dart';
import '../../../onboarding/application/add_lamp_notifier.dart';
import '../../../onboarding/presentation/widgets/confirm_add_dialog.dart';

Future<void> showLampPickerSheet(
  BuildContext context, {
  required String currentLampId,
}) {
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    backgroundColor: BrandColors.midnightBlack,
    shape: const RoundedRectangleBorder(
      borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
    ),
    builder: (ctx) => FractionallySizedBox(
      heightFactor: 0.75,
      child: LampPickerSheet(currentLampId: currentLampId),
    ),
  );
}

class LampPickerSheet extends ConsumerWidget {
  const LampPickerSheet({super.key, required this.currentLampId});

  final String currentLampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final inventory =
        ref.watch(inventoryNotifierProvider).value ?? const [];
    final nearby = ref.watch(nearbyLampsNotifierProvider);
    final inventoryIds = inventory.map((l) => l.id).toSet();
    final unknownNearby =
        nearby.where((n) => !inventoryIds.contains(n.id)).toList();

    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            Row(
              children: [
                const Text(
                  'Your lamps',
                  style: TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 16,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const Spacer(),
                IconButton(
                  icon: const Icon(Icons.close, color: BrandColors.slateGrey),
                  onPressed: () => Navigator.pop(context),
                  tooltip: 'Close',
                ),
              ],
            ),
            const SizedBox(height: 8),
            Expanded(
              child: ListView(
                children: [
                  for (final l in inventory)
                    _InventoryLampTile(
                      lamp: l,
                      isCurrent: l.id == currentLampId,
                      nearby: nearby,
                    ),
                  if (unknownNearby.isNotEmpty) ...[
                    const SizedBox(height: 16),
                    const Padding(
                      padding: EdgeInsets.symmetric(horizontal: 4),
                      child: Text(
                        'Other nearby lamps',
                        style: TextStyle(
                          color: BrandColors.fogGrey,
                          fontSize: 12,
                          letterSpacing: 0.5,
                        ),
                      ),
                    ),
                    const SizedBox(height: 8),
                    for (final n in unknownNearby)
                      _NearbyLampTile(lamp: n),
                  ],
                ],
              ),
            ),
            const Divider(color: BrandColors.slateGrey, height: 1),
            const SizedBox(height: 4),
            TextButton.icon(
              icon: const Icon(Icons.add, size: 18),
              label: const Text('Add a lamp'),
              onPressed: () {
                Navigator.pop(context);
                GoRouter.maybeOf(context)?.go(AppRoutes.addLamp);
              },
            ),
          ],
        ),
      ),
    );
  }
}

class _InventoryLampTile extends ConsumerWidget {
  const _InventoryLampTile({
    required this.lamp,
    required this.isCurrent,
    required this.nearby,
  });

  final InventoryLamp lamp;
  final bool isCurrent;
  final List<NearbyLamp> nearby;

  Color? _colorFromList(List<int>? rgb) {
    if (rgb == null || rgb.length < 3) return null;
    return Color.fromARGB(0xFF, rgb[0], rgb[1], rgb[2]);
  }

  Future<void> _onTap(BuildContext context, WidgetRef ref) async {
    if (isCurrent) {
      Navigator.pop(context);
      return;
    }
    await ref.read(activeLampNotifierProvider.notifier).set(lamp.id);
    if (context.mounted) {
      Navigator.pop(context);
      // Only navigate if we're inside a GoRouter tree. Tests that pump the
      // sheet directly (without GoRouter) skip the route change — the
      // active-lamp state change is what matters in that scenario.
      GoRouter.maybeOf(context)?.go(AppRoutes.control(lamp.id));
    }
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    // The currently-active lamp's row shows mesh when we're connected to
    // it; every other row's connected flag is false from this screen's
    // perspective (we only hold one BLE connection at a time).
    final connected = isCurrent &&
        (ref.watch(controlNotifierProvider(lamp.id)).value?.connected ??
            false);
    final status = statusFor(
      lampId: lamp.id,
      nearby: nearby,
      connected: connected,
    );
    return InkWell(
      borderRadius: BorderRadius.circular(12),
      onTap: () => _onTap(context, ref),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 10),
        child: Row(
          children: [
            StatusDot(kind: status),
            const SizedBox(width: 12),
            LampIcon(
              shade: _colorFromList(lamp.lastShadeColor),
              base: _colorFromList(lamp.lastBaseColor),
              size: 36,
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                lamp.name,
                style: const TextStyle(
                  color: BrandColors.lampWhite,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
            if (isCurrent)
              Container(
                padding: const EdgeInsets.symmetric(
                  horizontal: 8,
                  vertical: 4,
                ),
                decoration: BoxDecoration(
                  borderRadius: BorderRadius.circular(999),
                  color: BrandColors.lumenGreen.withValues(alpha: 0.18),
                ),
                child: const Text(
                  'active',
                  style: TextStyle(
                    fontSize: 10,
                    color: BrandColors.lumenGreen,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              )
            else
              const Icon(Icons.chevron_right, color: BrandColors.slateGrey),
          ],
        ),
      ),
    );
  }
}

class _NearbyLampTile extends ConsumerWidget {
  const _NearbyLampTile({required this.lamp});

  final NearbyLamp lamp;

  Color _colorFromInt(int rgb) => Color.fromARGB(
        0xFF,
        (rgb >> 16) & 0xFF,
        (rgb >> 8) & 0xFF,
        rgb & 0xFF,
      );

  Future<void> _onTap(BuildContext context, WidgetRef ref) async {
    if (lamp.isFactoryDefault) {
      await ref.read(addLampNotifierProvider.notifier).select(lamp.id);
      if (context.mounted) {
        Navigator.pop(context);
        GoRouter.maybeOf(context)?.go(AppRoutes.addLamp);
      }
    } else {
      final confirmed = await confirmAddDialog(context, lamp.name);
      if (!confirmed) return;
      if (!context.mounted) return;
      await ref
          .read(addLampNotifierProvider.notifier)
          .add(deviceId: lamp.id, name: lamp.name);
      if (context.mounted) Navigator.pop(context);
    }
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    return InkWell(
      borderRadius: BorderRadius.circular(12),
      onTap: () => _onTap(context, ref),
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 10),
        child: Row(
          children: [
            const StatusDot(kind: StatusKind.bluetooth),
            const SizedBox(width: 12),
            LampIcon(
              shade: _colorFromInt(lamp.shadeRgb),
              base: _colorFromInt(lamp.baseRgb),
              size: 36,
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                lamp.name.isEmpty ? '(unnamed)' : lamp.name,
                style: const TextStyle(
                  color: BrandColors.lampWhite,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
            _NearbyPill(factoryDefault: lamp.isFactoryDefault),
          ],
        ),
      ),
    );
  }
}

class _NearbyPill extends StatelessWidget {
  const _NearbyPill({required this.factoryDefault});

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
