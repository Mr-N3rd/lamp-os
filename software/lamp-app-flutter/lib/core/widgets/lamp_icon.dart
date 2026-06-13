import 'package:flutter/material.dart';

import '../theme/brand_colors.dart';

class LampIcon extends StatelessWidget {
  const LampIcon({
    super.key,
    this.shade,
    this.base,
    this.size = 26,
  });

  final Color? shade;
  final Color? base;
  final double size;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: size,
      height: size,
      child: CustomPaint(
        painter: _LampPainter(
          shade: shade ?? BrandColors.slateGrey,
          base: base ?? BrandColors.slateGrey,
        ),
      ),
    );
  }
}

class _LampPainter extends CustomPainter {
  _LampPainter({required this.shade, required this.base});

  final Color shade;
  final Color base;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width;
    final h = size.height;

    // Trapezoid shade (top ~58% of icon)
    final shadePaint = Paint()..color = shade;
    final shadePath = Path()
      ..moveTo(w * 0.22, h * 0.06)
      ..lineTo(w * 0.78, h * 0.06)
      ..lineTo(w * 0.92, h * 0.58)
      ..lineTo(w * 0.08, h * 0.58)
      ..close();
    canvas.drawPath(shadePath, shadePaint);

    // Neck
    final basePaint = Paint()..color = base;
    canvas.drawRect(
      Rect.fromLTWH(w * 0.44, h * 0.58, w * 0.12, h * 0.18),
      basePaint,
    );

    // Base disc
    canvas.drawRRect(
      RRect.fromRectAndRadius(
        Rect.fromLTWH(w * 0.25, h * 0.76, w * 0.50, h * 0.16),
        Radius.circular(w * 0.04),
      ),
      basePaint,
    );
  }

  @override
  bool shouldRepaint(_LampPainter old) =>
      old.shade != shade || old.base != base;
}
