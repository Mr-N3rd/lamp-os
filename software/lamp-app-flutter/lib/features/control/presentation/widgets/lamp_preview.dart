import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_svg/flutter_svg.dart';

import '../../../inventory/application/inventory_notifier.dart';
import '../../domain/lamp_color.dart';
import 'critter_asset.dart';

/// A small recolored lamp graphic that reflects the current shade + base
/// gradient in real time. Picks the same critter SVG as ConnectingView for
/// this lamp (driven by the persistent `InventoryLamp.critterIndex`).
///
/// Each viable critter SVG carries two linearGradient defs whose ids end in
/// `Shade` and `Body`. On each rebuild the `<linearGradient …>…</linearGradient>`
/// blocks in the cached template are replaced with new blocks whose
/// `<stop>` elements carry the live colors.
class LampPreview extends ConsumerStatefulWidget {
  const LampPreview({
    super.key,
    required this.deviceId,
    required this.shade,
    required this.baseColors,
    this.size = 140,
  });

  final String deviceId;
  final LampColor shade;
  final List<LampColor> baseColors;
  final double size;

  @override
  ConsumerState<LampPreview> createState() => _LampPreviewState();
}

class _LampPreviewState extends ConsumerState<LampPreview> {
  // Cache one template per asset path so the same SVG isn't re-read each
  // time a different lamp's preview mounts.
  static final Map<String, String> _templates = {};

  String? _localTemplate;
  String? _loadedFor; // the asset path the local template was loaded from

  Future<void> _ensureLoaded(String assetPath) async {
    final cached = _templates[assetPath];
    if (cached != null) {
      if (_loadedFor != assetPath) {
        setState(() {
          _localTemplate = cached;
          _loadedFor = assetPath;
        });
      }
      return;
    }
    final s = await rootBundle.loadString(assetPath);
    _templates[assetPath] = s;
    if (!mounted) return;
    setState(() {
      _localTemplate = s;
      _loadedFor = assetPath;
    });
  }

  /// Build replacement `<stop>` elements for [colors], spreading them evenly
  /// from 0 % to 100 % along the gradient axis.
  String _buildStops(List<LampColor> colors) {
    if (colors.isEmpty) {
      return '<stop offset="0%" stop-color="#000000"/>'
          '<stop offset="100%" stop-color="#000000"/>';
    }
    if (colors.length == 1) {
      final hex = colors.single.toHex().substring(1, 7);
      return '<stop offset="0%" stop-color="#$hex"/>'
          '<stop offset="100%" stop-color="#$hex"/>';
    }
    final n = colors.length;
    final buf = StringBuffer();
    for (var i = 0; i < n; i++) {
      final pct = (i / (n - 1) * 100).round();
      final hex = colors[i].toHex().substring(1, 7);
      buf.write('<stop offset="$pct%" stop-color="#$hex"/>');
    }
    return buf.toString();
  }

  /// Substitute the gradient blocks in [template] with the live colors from
  /// the current widget state.
  String _renderSvg(String template) {
    final shadeHex = widget.shade.toHex().substring(1, 7);
    final shadeStops = '<stop offset="0%" stop-color="#$shadeHex"/>'
        '<stop offset="100%" stop-color="#$shadeHex"/>';
    final bodyStops = _buildStops(widget.baseColors);

    final shadePattern = RegExp(
      r'<linearGradient[^>]*id="[^"]*Shade"[^>]*>.*?</linearGradient>',
      dotAll: true,
    );
    final bodyPattern = RegExp(
      r'<linearGradient[^>]*id="[^"]*Body"[^>]*>.*?</linearGradient>',
      dotAll: true,
    );

    return template
        .replaceFirstMapped(shadePattern, (m) {
          final tag = m.group(0)!;
          final openTag = '${tag.split('>').first}>';
          return '$openTag$shadeStops</linearGradient>';
        })
        .replaceFirstMapped(bodyPattern, (m) {
          final tag = m.group(0)!;
          final openTag = '${tag.split('>').first}>';
          return '$openTag$bodyStops</linearGradient>';
        });
  }

  @override
  Widget build(BuildContext context) {
    final inventory = ref.watch(inventoryNotifierProvider).value;
    final lamp =
        inventory?.firstWhereOrNull((l) => l.id == widget.deviceId);
    final asset = critterAssetFor(
      critterIndex: lamp?.critterIndex,
      deviceId: widget.deviceId,
    );

    // Kick off (or refresh) the load if this is a new asset path. We do it
    // here rather than initState because the chosen asset can change if the
    // inventory entry's critterIndex appears asynchronously.
    if (_loadedFor != asset) {
      _ensureLoaded(asset);
    }

    final template = _localTemplate;
    if (template == null) {
      return SizedBox(width: widget.size, height: widget.size);
    }
    return SizedBox(
      width: widget.size,
      height: widget.size,
      child: SvgPicture.string(
        _renderSvg(template),
        width: widget.size,
        height: widget.size,
      ),
    );
  }
}
