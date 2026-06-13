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

  // Compiled once per process; the patterns are constant across builds.
  static final RegExp _shadePattern = RegExp(
    r'<linearGradient[^>]*id="[^"]*Shade"[^>]*>.*?</linearGradient>',
    dotAll: true,
  );
  static final RegExp _bodyPattern = RegExp(
    r'<linearGradient[^>]*id="[^"]*Body"[^>]*>.*?</linearGradient>',
    dotAll: true,
  );

  String? _localTemplate;
  String? _loadedFor; // the asset path the local template was loaded from

  // Single-entry memoization for `_renderSvg`. Each rebuild that hits the
  // same shade + base color set returns the cached SVG string instead of
  // running three regex passes again. The cache key is the joined hex of
  // the inputs; values are cheap to compute and short.
  String? _memoKey;
  String? _memoRendered;
  String? _memoFor; // the template the cached result was rendered against

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
  /// from 0 % to 100 % along the gradient axis. Uses the `style="stop-color:…"`
  /// form to match the convention in the critter SVGs — flutter_svg renders
  /// the style-attribute form reliably but ignores `stop-color="…"` written
  /// as a direct attribute, which would leave the gradient uncolored.
  String _stopTag(double pct, String hex) =>
      '<stop offset="${pct.round()}%" '
      'style="stop-color:#$hex;stop-opacity:1"/>';

  String _buildStops(List<LampColor> colors) {
    if (colors.isEmpty) {
      return _stopTag(0, '000000') + _stopTag(100, '000000');
    }
    if (colors.length == 1) {
      final hex = colors.single.toRgbHex();
      return _stopTag(0, hex) + _stopTag(100, hex);
    }
    final n = colors.length;
    final buf = StringBuffer();
    for (var i = 0; i < n; i++) {
      final pct = i / (n - 1) * 100;
      final hex = colors[i].toRgbHex();
      buf.write(_stopTag(pct, hex));
    }
    return buf.toString();
  }

  /// Substitute the gradient blocks in [template] with the live colors from
  /// the current widget state.
  String _renderSvg(String template) {
    final shadeHex = widget.shade.toRgbHex();
    final shadeStops = _stopTag(0, shadeHex) + _stopTag(100, shadeHex);
    final bodyStops = _buildStops(widget.baseColors);

    String rewrite(String tag, String stops) {
      final openTag = '${tag.split('>').first}>';
      return '$openTag$stops</linearGradient>';
    }

    var out = template;
    out = out.replaceFirstMapped(
        _shadePattern, (m) => rewrite(m.group(0)!, shadeStops));
    out = out.replaceFirstMapped(
        _bodyPattern, (m) => rewrite(m.group(0)!, bodyStops));
    return out;
  }

  @override
  Widget build(BuildContext context) {
    // Watch ONLY this lamp's critterIndex. The previous `ref.watch` against
    // the whole inventory list caused a full LampPreview rebuild every time
    // setShadeColor / setBaseColors called _updateSeen (which writes
    // lastShadeColor + lastBaseColor back into inventory). Per slider tick
    // that storm of rebuilds was wasting frames re-rendering the same SVG.
    final critterIndex =
        ref.watch(inventoryNotifierProvider.select((async) {
      return async.value
          ?.firstWhereOrNull((l) => l.id == widget.deviceId)
          ?.critterIndex;
    }));
    final asset = critterAssetFor(
      critterIndex: critterIndex,
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
    final cacheKey =
        '${widget.shade.toHex()}|${widget.baseColors.map((c) => c.toHex()).join(",")}';
    String rendered;
    if (_memoFor == template &&
        _memoKey == cacheKey &&
        _memoRendered != null) {
      rendered = _memoRendered!;
    } else {
      rendered = _renderSvg(template);
      _memoFor = template;
      _memoKey = cacheKey;
      _memoRendered = rendered;
    }
    return SizedBox(
      width: widget.size,
      height: widget.size,
      // Key on the rendered content so flutter_svg treats each unique
      // shade/base color set as a brand-new picture. Without this, the
      // SvgPicture widget is reused across rebuilds and flutter_svg's
      // internal picture cache hands back the first decode even when our
      // source string differs.
      child: SvgPicture.string(
        rendered,
        key: ValueKey<String>(cacheKey),
        width: widget.size,
        height: widget.size,
      ),
    );
  }
}
