import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/routing/routes.dart';
import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/friendly_error.dart';
import 'package:go_router/go_router.dart' as gr;
import '../../nearby/application/nearby_lamps_notifier.dart';
import '../application/control_notifier.dart';
import '../application/lamp_auth_required_exception.dart';
import '../domain/lamp_color.dart';
import 'widgets/base_card.dart';
import 'widgets/base_editor_sheet.dart';
import 'widgets/brightness_card.dart';
import 'widgets/connect_password_prompt.dart';
import 'widgets/connecting_view.dart';
import 'widgets/connection_banner.dart';
import 'widgets/lamp_preview.dart';
import 'widgets/shade_card.dart';


class ControlScreen extends ConsumerStatefulWidget {
  const ControlScreen({super.key, required this.lampId});
  final String lampId;

  @override
  ConsumerState<ControlScreen> createState() => _ControlScreenState();
}

class _ControlScreenState extends ConsumerState<ControlScreen> {
  /// Defense-in-depth: if the lamp's most recent adv reports
  /// `isMesh: false` AND the BLE connection attempt failed (not auth-
  /// required — a real connect/handshake failure), bounce the user to
  /// the BT-only screen. Auth failures keep us here so the password
  /// prompt can surface; successful connections keep us here regardless
  /// of the cached adv (the adv may simply be stale).
  ///
  /// One-shot: we redirect at most once per ControlScreen instance so a
  /// transient adv drop later in the session can't yank the user away.
  bool _btOnlyRedirectChecked = false;

  @override
  Widget build(BuildContext context) {
    final lampId = widget.lampId;
    final async = ref.watch(controlNotifierProvider(lampId));
    if (!_btOnlyRedirectChecked && async is AsyncError) {
      final err = async.error;
      if (err is! LampAuthRequiredException) {
        final isMesh = ref.read(nearbyLampsNotifierProvider).firstWhereOrNull(
              (l) => l.id == lampId,
            )?.isMesh;
        final router = GoRouter.maybeOf(context);
        if (isMesh == false && router != null) {
          _btOnlyRedirectChecked = true;
          // pushReplacement during build is illegal — defer to post-frame.
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (!mounted) return;
            router.pushReplacement(AppRoutes.btOnly(lampId));
          });
          return ConnectingView(deviceId: lampId);
        }
      }
    }
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
                        // LampPreview watches its own (shade, base) slice
                        // — only rebuilds when those change, not when the
                        // surrounding state churns from coalesced writes
                        // or inventory writebacks.
                        LampPreview(
                          deviceId: lampId,
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
                  // ShadeCard / BaseCard watch their own slices; ControlScreen
                  // hands them lampId + the edit-session callback. The
                  // onChanged write path is internalised in the card.
                  ShadeCard(
                    lampId: lampId,
                    onEditSessionChanged: (open) =>
                        notifier.setEditSession(EditSurface.shade, open),
                  ),
                  BaseCard(
                    lampId: lampId,
                    onTap: () =>
                        showBaseEditorSheet(context, lampId: lampId),
                  ),
                  const SizedBox(height: 12),
                  BrightnessCard(
                    lampId: lampId,
                    onEditSessionChanged: (open) => notifier
                        .setEditSession(EditSurface.brightness, open),
                  ),
                  const SizedBox(height: 8),
                  _ConfigurationRow(lampId: lampId),
                ],
              ),
            ),
          ],
        );
      },
    );
  }
}

/// Inline "Lamp configuration" row at the bottom of the Setup tab body.
/// Replaces the AppBar gear that used to live on every tab — the gear
/// felt like chrome bolted on top; an inline row reads as "this is just
/// the next thing in Setup, after the colour cards." Navigates to the
/// same `/lamp/:id/setup` route (lamp name, password, home mode,
/// advanced LEDs, About / firmware update / version footer).
class _ConfigurationRow extends StatelessWidget {
  const _ConfigurationRow({required this.lampId});

  final String lampId;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: () =>
            gr.GoRouter.maybeOf(context)?.push(AppRoutes.setup(lampId)),
        child: Container(
          padding: const EdgeInsets.all(16),
          decoration: BoxDecoration(
            color: Colors.white.withValues(alpha: 0.04),
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: Colors.white.withValues(alpha: 0.06)),
          ),
          child: const Row(
            children: [
              Icon(Icons.settings, color: BrandColors.lampWhite, size: 22),
              SizedBox(width: 16),
              Expanded(
                child: Text(
                  'Configuration',
                  style: TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                    letterSpacing: 0.4,
                  ),
                ),
              ),
              Icon(Icons.chevron_right, color: BrandColors.slateGrey),
            ],
          ),
        ),
      ),
    );
  }
}
