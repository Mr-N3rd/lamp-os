import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../application/control_notifier.dart';
import '../domain/lamp_color.dart';
import 'widgets/base_card.dart';
import 'widgets/base_editor_sheet.dart';
import 'widgets/brightness_card.dart';
import 'widgets/connecting_view.dart';
import 'widgets/connection_banner.dart';
import 'widgets/lamp_preview.dart';
import 'widgets/shade_card.dart';

const _blackShade = LampColor(r: 0, g: 0, b: 0, w: 0);

class ControlScreen extends ConsumerWidget {
  const ControlScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final async = ref.watch(controlNotifierProvider(lampId));
    return async.when(
      loading: () => ConnectingView(deviceId: lampId),
      error: (e, _) => Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Text(
            'Could not reach this lamp: $e',
            style: const TextStyle(color: BrandColors.fogGrey),
            textAlign: TextAlign.center,
          ),
        ),
      ),
      data: (state) {
        final notifier = ref.read(controlNotifierProvider(lampId).notifier);
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
                  BrightnessCard(
                    value: state.lamp.brightness,
                    onChanged: notifier.setBrightness,
                  ),
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
                ],
              ),
            ),
          ],
        );
      },
    );
  }
}
