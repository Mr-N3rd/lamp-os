import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/theme/brand_colors.dart';
import '../../../core/widgets/info_panel.dart';
import '../../control/application/control_notifier.dart';
import '../../control/presentation/widgets/connecting_view.dart';

/// Advanced LED settings — number of pixels per strip + strip-type (LED
/// byte order). Unlocked from the Info tab via the 5-tap secret on the
/// lamplit wordmark, or directly toggleable from the Setup screen once
/// advanced is on.
///
/// Strip-type options:
///   - **RGBW** — 4 bpp NeoPixel (`NEO_GRBW` on the wire). Has a true
///     warm-white channel.
///   - **RGB** — 3 bpp NeoPixel in standard `NEO_GRB` byte order, no W.
///   - **BGR** — 3 bpp NeoPixel in `NEO_BGR` order. Requires a firmware
///     option that isn't yet plumbed; shown disabled with an info note
///     until that lands.
class AdvancedLedsScreen extends ConsumerWidget {
  const AdvancedLedsScreen({super.key, required this.lampId});
  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final async = ref.watch(controlNotifierProvider(lampId));
    return Scaffold(
      appBar: AppBar(
        leading: IconButton(
          icon: const Icon(Icons.arrow_back),
          onPressed: () => GoRouter.maybeOf(context)?.pop(),
        ),
        title: const Text('Advanced LED setup'),
      ),
      body: async.when(
        loading: () => ConnectingView(deviceId: lampId),
        error: (e, _) => Center(
          child: Text('Could not reach this lamp: $e',
              style: const TextStyle(color: BrandColors.fogGrey)),
        ),
        data: (state) {
          final notifier = ref.read(controlNotifierProvider(lampId).notifier);
          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              const InfoPanel(
                child: Text(
                  'Match these to the physical strips you built into the '
                  'lamp. Mis-matched LED count or byte-order will show as '
                  'wrong-coloured or partially-lit segments.',
                ),
              ),
              const SizedBox(height: 16),
              const Text(
                'Base strip',
                style: TextStyle(
                  color: BrandColors.headerYellow,
                  fontSize: 13,
                  fontWeight: FontWeight.w700,
                  letterSpacing: 0.8,
                ),
              ),
              const SizedBox(height: 8),
              _PixelCountField(
                initial: state.base.px,
                label: 'Base LED count',
                onChanged: notifier.setBasePx,
              ),
              const SizedBox(height: 12),
              SegmentedButton<int>(
                showSelectedIcon: false,
                segments: const [
                  ButtonSegment(value: 4, label: Text('RGBW')),
                  ButtonSegment(value: 3, label: Text('RGB')),
                ],
                selected: {state.base.bpp},
                onSelectionChanged: (s) => notifier.setBaseBpp(s.first),
              ),
              const SizedBox(height: 24),
              const Text(
                'Shade strip',
                style: TextStyle(
                  color: BrandColors.headerYellow,
                  fontSize: 13,
                  fontWeight: FontWeight.w700,
                  letterSpacing: 0.8,
                ),
              ),
              const SizedBox(height: 8),
              _PixelCountField(
                initial: state.shade.px,
                label: 'Shade LED count',
                onChanged: notifier.setShadePx,
              ),
              const SizedBox(height: 12),
              SegmentedButton<int>(
                showSelectedIcon: false,
                segments: const [
                  ButtonSegment(value: 4, label: Text('RGBW')),
                  ButtonSegment(value: 3, label: Text('RGB')),
                ],
                selected: {state.shade.bpp},
                onSelectionChanged: (s) => notifier.setShadeBpp(s.first),
              ),
              const SizedBox(height: 24),
              const InfoPanel(
                child: Text(
                  'BGR-byte-order strips are a planned firmware option — '
                  'not selectable yet. If your strips show swapped red/blue '
                  'channels, hold tight; a firmware update will let you '
                  'flip the byte order here.',
                ),
              ),
              const SizedBox(height: 24),
              SwitchListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Advanced enabled',
                    style: TextStyle(
                        color: BrandColors.lampWhite, fontSize: 14)),
                subtitle: const Text(
                  'Turning this off hides the advanced LED settings until '
                  'the 5-tap unlock is performed again.',
                  style: TextStyle(
                      color: BrandColors.fogGrey, fontSize: 12),
                ),
                value: state.lamp.advancedEnabled,
                onChanged: notifier.setLampAdvancedEnabled,
              ),
            ],
          );
        },
      ),
    );
  }
}

class _PixelCountField extends StatefulWidget {
  const _PixelCountField({
    required this.initial,
    required this.label,
    required this.onChanged,
  });
  final int initial;
  final String label;
  final ValueChanged<int> onChanged;

  @override
  State<_PixelCountField> createState() => _PixelCountFieldState();
}

class _PixelCountFieldState extends State<_PixelCountField> {
  late final _ctrl = TextEditingController(text: '${widget.initial}');

  @override
  void didUpdateWidget(_PixelCountField old) {
    super.didUpdateWidget(old);
    if (old.initial != widget.initial && !_ctrl.value.composing.isValid) {
      _ctrl.text = '${widget.initial}';
    }
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return TextField(
      controller: _ctrl,
      keyboardType: TextInputType.number,
      style: const TextStyle(color: BrandColors.lampWhite),
      decoration: InputDecoration(labelText: widget.label),
      onChanged: (v) {
        final n = int.tryParse(v);
        if (n != null && n > 0 && n <= 255) widget.onChanged(n);
      },
    );
  }
}
