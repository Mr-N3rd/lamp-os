import 'package:flutter/material.dart';

class ControlPlaceholder extends StatelessWidget {
  const ControlPlaceholder({super.key, required this.lampId});
  final String lampId;
  @override
  Widget build(BuildContext context) =>
      Center(child: Text('Control · $lampId'));
}
