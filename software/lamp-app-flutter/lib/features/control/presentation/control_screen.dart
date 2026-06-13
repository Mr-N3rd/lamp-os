import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/friendly_error.dart';
import '../../nearby/application/nearby_lamps_notifier.dart';
import '../application/control_notifier.dart';
import '../application/lamp_auth_required_exception.dart';
import '../domain/lamp_color.dart';
import 'widgets/base_card.dart';
import 'widgets/base_editor_sheet.dart';
import 'widgets/brightness_card.dart';
import 'widgets/bt_only_info_pane.dart';
import 'widgets/connect_password_prompt.dart';
import 'widgets/connecting_view.dart';
import 'widgets/connection_banner.dart';
import 'widgets/lamp_preview.dart';
import 'widgets/shade_card.dart';

const _blackShade = LampColor(r: 0, g: 0, b: 0, w: 0);

class ControlScreen extends ConsumerStatefulWidget {
  const ControlScreen({super.key, required this.lampId});
  final String lampId;

  @override
  ConsumerState<ControlScreen> createState() => _ControlScreenState();
}

class _ControlScreenState extends ConsumerState<ControlScreen> {
  /// Defense-in-depth: if the user somehow landed here for a lamp whose
  /// adv reports `isMesh: false` (legacy/BT-only firmware), bounce them
  /// to the BT-only pane instead of letting the control flow trap them
  /// on a ConnectingView that will never complete the GATT handshake.
  /// We only act on the FIRST observation of `isMesh` so a transient
  /// adv drop later in the session doesn't yank the user away.
  bool _initialMeshChecked = false;

  @override
  Widget build(BuildContext context) {
    final lampId = widget.lampId;
    final isMesh = ref.watch(nearbyLampsNotifierProvider.select(
      (list) => list.firstWhereOrNull((l) => l.id == lampId)?.isMesh,
    ));
    if (!_initialMeshChecked && isMesh != null) {
      _initialMeshChecked = true;
      // Only short-circuit when we can actually navigate. Without a
      // GoRouter in the tree (some widget tests pump ControlScreen
      // directly), fall through so the embedded BtOnlyInfoPane below
      // can still render as the secondary surface.
      final router = GoRouter.maybeOf(context);
      if (isMesh == false && router != null) {
        // Schedule the redirect post-frame — pushReplacement during
        // build is illegal. Once-only via the flag above.
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (!mounted) return;
          router.pushReplacement(AppRoutes.btOnly(lampId));
        });
        return ConnectingView(deviceId: lampId);
      }
    }
    final async = ref.watch(controlNotifierProvider(lampId));
    return async.when(
      loading: () => ConnectingView(deviceId: lampId),
      error: (e, _) {
        if (e is LampAuthRequiredException) {
          return ConnectPasswordPrompt(lampId: lampId);
        }
        return FriendlyError.page(
          title: "Couldn't reach your lamp.",
          subtitle:
              "They may have wandered out of range. Bring your phone closer "
              'and try again.',
          rawError: e,
        );
      },
      data: (state) {
        final notifier =
            ref.read(controlNotifierProvider(lampId).notifier);
        final shade = state.shade.colors.isEmpty
            ? _blackShade
            : state.shade.colors.single;
        return Column(
          children: [
            if (!state.connected)
              ConnectionBanner(attempt: state.reconnectAttempt),
            Expanded(
              child: ListView(
                padding: const EdgeInsets.symmetric(vertical: 8),
                children: [
                  // "Hello my name is:" nameplate beside the live critter
                  // — ports `CritterNameplate.vue` from the old UI.
                  Padding(
                    padding: const EdgeInsets.symmetric(
                        vertical: 24, horizontal: 16),
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.center,
                      children: [
                        LampPreview(
                          deviceId: lampId,
                          shade: shade,
                          baseColors: state.base.colors,
                          // Smaller than the previous centred 140 so the
                          // name text gets enough room to the right.
                          size: 100,
                        ),
                        const SizedBox(width: 24),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              const Text(
                                'Hello my name is:',
                                style: TextStyle(
                                  color: BrandColors.nameplateGrey,
                                  fontSize: 13,
                                  fontWeight: FontWeight.w300,
                                ),
                              ),
                              Text(
                                state.lamp.name,
                                style: const TextStyle(
                                  color: BrandColors.lampWhite,
                                  fontSize: 28,
                                  fontWeight: FontWeight.w800,
                                  height: 1.0,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                  ),
                  BtOnlyInfoPane(lampId: lampId),
                  ShadeCard(
                    color: shade,
                    bpp: state.shade.bpp,
                    onChanged: notifier.setShadeColor,
                  ),
                  BaseCard(
                    colors: state.base.colors,
                    activeIndex: state.base.ac,
                    onTap: () =>
                        showBaseEditorSheet(context, lampId: lampId),
                  ),
                  const SizedBox(height: 12),
                  BrightnessCard(
                    value: state.lamp.brightness,
                    onChanged: notifier.setBrightness,
                  ),
                ],
              ),
            ),
          ],
        );
      },
    );
  }
}
