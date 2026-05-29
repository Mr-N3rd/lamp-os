import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/theme/brand_colors.dart';
import '../application/control_notifier.dart';
import '../domain/lamp_color.dart';
import 'widgets/base_card.dart';
import 'widgets/base_editor_sheet.dart';
import 'widgets/brightness_card.dart';
import 'widgets/connecting_view.dart';
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
        return ListView(
          padding: const EdgeInsets.symmetric(vertical: 8),
          children: [
            BrightnessCard(
              value: state.lamp.brightness,
              onChanged: notifier.setBrightness,
            ),
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 8),
              child: Center(
                child: LampPreview(
                  deviceId: lampId,
                  shade: shade,
                  baseColors: state.base.colors,
                ),
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
              onTap: () => showBaseEditorSheet(context, lampId: lampId),
            ),
          ],
        );
      },
    );
  }
}
