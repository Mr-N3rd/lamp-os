import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'core/ble/ble_permissions.dart';
import 'core/routing/router.dart';
import 'core/theme/app_theme.dart';
import 'core/theme/brand_colors.dart';

class LampApp extends ConsumerStatefulWidget {
  const LampApp({super.key, BlePermissions? permissions})
      : _injected = permissions;
  final BlePermissions? _injected;

  @override
  ConsumerState<LampApp> createState() => _LampAppState();
}

class _LampAppState extends ConsumerState<LampApp> {
  late final BlePermissions _perms =
      widget._injected ?? AndroidBlePermissions();
  Future<bool>? _granted;

  @override
  void initState() {
    super.initState();
    _granted = _perms.request();
  }

  void _retry() {
    setState(() {
      _granted = _perms.request();
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Lamplit',
      theme: AppTheme.dark(),
      debugShowCheckedModeBanner: false,
      home: FutureBuilder<bool>(
        future: _granted,
        builder: (context, snap) {
          if (snap.connectionState != ConnectionState.done) {
            return const Scaffold(
              body: Center(child: CircularProgressIndicator()),
            );
          }
          if (snap.data != true) {
            return Scaffold(
              body: Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Text(
                      'Bluetooth permission needed',
                      style: TextStyle(
                        color: BrandColors.lampWhite,
                        fontWeight: FontWeight.w600,
                        fontSize: 18,
                      ),
                    ),
                    const SizedBox(height: 12),
                    const Text(
                      'Lamplit talks to your lamps over Bluetooth.',
                      style: TextStyle(color: BrandColors.fogGrey),
                    ),
                    const SizedBox(height: 24),
                    FilledButton(
                      onPressed: _retry,
                      child: const Text('Allow Bluetooth'),
                    ),
                  ],
                ),
              ),
            );
          }
          final router = ref.watch(appRouterProvider);
          return MaterialApp.router(
            title: 'Lamplit',
            theme: AppTheme.dark(),
            routerConfig: router,
            debugShowCheckedModeBanner: false,
          );
        },
      ),
    );
  }
}
