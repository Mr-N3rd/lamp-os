import 'package:flutter/material.dart';

class SetupPlaceholder extends StatelessWidget {
  const SetupPlaceholder({super.key, required this.lampId});
  final String lampId;
  @override
  Widget build(BuildContext context) =>
      Center(child: Text('Setup · $lampId'));
}
