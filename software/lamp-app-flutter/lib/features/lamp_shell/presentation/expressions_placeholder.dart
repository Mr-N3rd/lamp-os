import 'package:flutter/material.dart';

class ExpressionsPlaceholder extends StatelessWidget {
  const ExpressionsPlaceholder({super.key, required this.lampId});
  final String lampId;
  @override
  Widget build(BuildContext context) =>
      Center(child: Text('Expressions · $lampId'));
}
