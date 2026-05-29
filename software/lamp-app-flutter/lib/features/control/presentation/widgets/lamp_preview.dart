import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:flutter_svg/flutter_svg.dart';

import '../../domain/lamp_color.dart';

/// A small recolored lamp graphic that reflects the current shade + base
/// gradient in real time.
///
/// The underlying SVG is critter-3.svg which carries two linearGradient defs:
///   `critter3Shade` — drives the top/hood of the lamp
///   `critter3Body`  — drives the base
///
/// On each rebuild the `<linearGradient …>…</linearGradient>` blocks in the
/// cached template are replaced with new blocks whose `<stop>` elements carry
/// the live colors.
class LampPreview extends StatefulWidget {
  const LampPreview({
    super.key,
    required this.shade,
    required this.baseColors,
    this.size = 140,
  });

  final LampColor shade;
  final List<LampColor> baseColors;
  final double size;

  @override
  State<LampPreview> createState() => _LampPreviewState();
}

class _LampPreviewState extends State<LampPreview> {
  // Shared across all instances so the file is only read once per app session.
  static String? _template;

  // Instance-local copy so setState can trigger a rebuild after async load.
  String? _localTemplate;

  @override
  void initState() {
    super.initState();
    if (_template != null) {
      _localTemplate = _template;
    } else {
      rootBundle.loadString('assets/lamp-preview/lamp.svg').then((s) {
        _template = s;
        if (mounted) setState(() => _localTemplate = s);
      });
    }
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
    final shadeStops =
        '<stop offset="0%" stop-color="#$shadeHex"/>'
        '<stop offset="100%" stop-color="#$shadeHex"/>';
    final bodyStops = _buildStops(widget.baseColors);

    // Matches any <linearGradient …id="…Shade"…>…</linearGradient> block.
    final shadePattern = RegExp(
      r'<linearGradient[^>]*id="[^"]*Shade"[^>]*>.*?</linearGradient>',
      dotAll: true,
    );
    // Matches any <linearGradient …id="…Body"…>…</linearGradient> block.
    final bodyPattern = RegExp(
      r'<linearGradient[^>]*id="[^"]*Body"[^>]*>.*?</linearGradient>',
      dotAll: true,
    );

    return template
        .replaceFirstMapped(shadePattern, (m) {
          final tag = m.group(0)!;
          // Re-use the original opening tag (preserves id, x1/y1/x2/y2 etc.)
          // but strip any existing child stops.
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
    final template = _localTemplate;
    if (template == null) {
      // SVG not yet loaded — reserve space so layout is stable.
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
