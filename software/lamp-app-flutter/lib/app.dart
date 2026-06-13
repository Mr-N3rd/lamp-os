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

class _LampAppState extends ConsumerState<LampApp>
    with WidgetsBindingObserver {
  late final BlePermissions _perms =
      widget._injected ?? BlePermissions.forPlatform();
  Future<bool>? _granted;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _granted = _perms.request();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    // App came back to foreground — if the user just toggled BT perm
    // in Settings (post-`openSettings`), the in-memory `_granted`
    // Future still resolves to false until we re-check. Without this,
    // the user grants the permission, switches back to the app, and
    // the screen still says "Allow Bluetooth" until they tap it.
    // (audit ux-C1)
    if (state == AppLifecycleState.resumed) {
      // Cheap: just re-runs request(). On iOS this is a status query
      // (no second system dialog), on Android it's also a no-op once
      // granted. The setState forces the FutureBuilder to re-evaluate.
      setState(() {
        _granted = _perms.isGranted();
      });
    }
  }

  void _retry() {
    setState(() {
      _granted = _requestOrOpenSettings();
    });
  }

  Future<bool> _requestOrOpenSettings() async {
    final granted = await _perms.request();
    if (granted) return true;
    // Android marks the permission "permanently denied" after the user has
    // dismissed the system prompt twice. iOS treats the first deny the
    // same way — only Settings can recover. From there, request() returns
    // false immediately without showing anything — the only path forward
    // is the app-settings screen.
    if (await _perms.isPermanentlyDenied()) {
      await _perms.openSettings();
      // didChangeAppLifecycleState picks up the resume and re-evaluates
      // anyway; the inline isGranted call here is the immediate response
      // for callers that don't go through the lifecycle path.
      return _perms.isGranted();
    }
    return false;
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'LampOS',
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
                      'LampOS talks to your lamps over Bluetooth.',
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
            title: 'LampOS',
            theme: AppTheme.dark(),
            routerConfig: router,
            debugShowCheckedModeBanner: false,
          );
        },
      ),
    );
  }
}
