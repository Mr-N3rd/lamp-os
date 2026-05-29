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
    )..repeat(reverse: true);
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final color = switch (widget.kind) {
      StatusKind.offline => BrandColors.slateGrey,
      StatusKind.bluetooth => const Color(0xFF4D6F53),
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
