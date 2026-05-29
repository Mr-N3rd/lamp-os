import 'package:flutter/material.dart';

/// Yes/No confirmation dialog used when the user taps an already-configured
/// nearby lamp (either from the AddLamp scan step or the lamp picker bottom
/// sheet) and we just need to register it in inventory without running the
/// claim wizard. Returns `true` on Add, `false` on Cancel or dismiss.
Future<bool> confirmAddDialog(BuildContext context, String name) async {
  final displayName = name.isEmpty ? 'this lamp' : '"$name"';
  final result = await showDialog<bool>(
    context: context,
    builder: (ctx) => AlertDialog(
      title: Text('Add $displayName to your lamps?'),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(ctx, false),
          child: const Text('Cancel'),
        ),
        FilledButton(
          onPressed: () => Navigator.pop(ctx, true),
          child: const Text('Add'),
        ),
      ],
    ),
  );
  return result ?? false;
}
