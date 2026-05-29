import 'package:flutter/material.dart';
import 'package:flutter_svg/flutter_svg.dart';

import '../../../../core/theme/brand_colors.dart';

const _critters = [
  'assets/critters/critter-1.svg',
  'assets/critters/critter-3.svg',
  'assets/critters/critter-5.svg',
  'assets/critters/critter-7.svg',
];

/// Full-screen "we're talking to the lamp" state — a critter SVG that gently
/// pulses, with a single line of text below. Picks a critter deterministically
/// from [deviceId] so the same lamp always shows the same friend.
class ConnectingView extends StatefulWidget {
  const ConnectingView({super.key, required this.deviceId});
  final String deviceId;

  @override
  State<ConnectingView> createState() => _ConnectingViewState();
}

class _ConnectingViewState extends State<ConnectingView>
    with SingleTickerProviderStateMixin {
  late final AnimationController _ctrl = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 1400),
  )..repeat(reverse: true);

  late final Animation<double> _scale = Tween(begin: 0.96, end: 1.04)
      .chain(CurveTween(curve: Curves.easeInOut))
      .animate(_ctrl);

  String get _critter {
    final i = widget.deviceId.codeUnits.fold<int>(0, (a, b) => a + b);
    return _critters[i.abs() % _critters.length];
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          ScaleTransition(
            scale: _scale,
            child: SvgPicture.asset(
              _critter,
              width: 160,
              height: 160,
            ),
          ),
          const SizedBox(height: 24),
          const Text(
            'Connecting…',
            style: TextStyle(
              color: BrandColors.fogGrey,
              fontSize: 14,
              letterSpacing: 0.5,
            ),
          ),
        ],
      ),
    );
  }
}
