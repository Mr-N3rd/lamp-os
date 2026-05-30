import 'package:flutter/material.dart';

import '../theme/brand_colors.dart';

enum StatusKind { offline, bluetooth, mesh }

class StatusDot extends StatefulWidget {
  const StatusDot({super.key, required this.kind, this.size = 10});

  final StatusKind kind;
  final double size;

  @override
  State<StatusDot> createState() => _StatusDotState();
}

class _StatusDotState extends State<StatusDot>
    with SingleTickerProviderStateMixin {
  late final AnimationController _ctrl;

  @override
  void initState() {
    super.initState();
    _ctrl = AnimationController(
      vsync: this,
      duration: const Duration(seconds: 2),
    );
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    // Pause the pulse when this widget is in an off-screen tab. Flutter's
    // TickerMode is `false` for descendants of a NavigationBar destination
    // that isn't currently visible; respecting it keeps a row of dots from
    // burning CPU when their lamp shell tab is hidden.
    if (TickerMode.getValuesNotifier(context).value.enabled) {
      if (!_ctrl.isAnimating) _ctrl.repeat(reverse: true);
    } else {
      _ctrl.stop();
    }
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Mesh-connected lamps glow brand green (mesh is the live, healthy
    // state). BT-only is auroraBlue at ~45 % so it reads as "online but not
    // the live link." Offline is a neutral grey.
    final color = switch (widget.kind) {
      StatusKind.offline => BrandColors.slateGrey,
      StatusKind.bluetooth => BrandColors.auroraBlue.withValues(alpha: 0.45),
      StatusKind.mesh => BrandColors.lumenGreen,
    };

    return AnimatedBuilder(
      animation: _ctrl,
      builder: (context, _) {
        final glow = widget.kind == StatusKind.mesh
            ? 6 + _ctrl.value * 8
            : (widget.kind == StatusKind.bluetooth ? 4.0 : 0.0);
        return Container(
          width: widget.size,
          height: widget.size,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            color: color,
            boxShadow: glow > 0
                ? [
                    BoxShadow(
                      color: color.withValues(alpha: 0.6),
                      blurRadius: glow,
                    ),
                  ]
                : const [],
          ),
        );
      },
    );
  }
}
