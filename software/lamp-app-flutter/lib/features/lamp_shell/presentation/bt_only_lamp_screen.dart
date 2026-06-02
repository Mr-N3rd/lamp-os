import 'package:collection/collection.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:url_launcher/url_launcher.dart';

import '../../../core/theme/brand_colors.dart';
import '../../inventory/application/inventory_notifier.dart';

/// Full-screen replacement for the old in-control banner. Shown when the
/// user taps a lamp from "My lamps" whose latest BLE advertisement reports
/// `isMesh: false` — i.e. the lamp's firmware predates the mesh + app
/// protocol. The app CAN'T control it. The lamp just exists in BLE
/// advertising range.
///
/// Two paths the user can take from here:
///
///   1. Configure / control it now — connect this phone's Wi-Fi to the
///      lamp's own access point (its name is the lamp's name), then open
///      `http://192.168.4.1` in a browser. That's the lamp's on-board web
///      UI. The address is shown copy-only (no tap-to-launch) because the
///      user needs to switch their Wi-Fi network first; jumping straight
///      to the URL while still on cellular would just fail.
///   2. Get app + mesh features — flash the latest firmware via
///      `https://update.lamplit.ca`. The URL is tappable — opens the
///      site in the system browser.
class BtOnlyLampScreen extends ConsumerWidget {
  const BtOnlyLampScreen({super.key, required this.lampId});

  final String lampId;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final inventory =
        ref.watch(inventoryNotifierProvider).value ?? const [];
    final lamp = inventory.firstWhereOrNull((l) => l.id == lampId);
    final name = lamp?.name ?? lampId;
    return Scaffold(
      appBar: AppBar(title: Text(name)),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 24),
          children: const [
            Text(
              "This lamp is in Bluetooth range but the app can't talk to "
              "it directly — its firmware doesn't speak the app's mesh "
              "protocol yet.",
              style: TextStyle(
                color: BrandColors.lampWhite,
                fontSize: 15,
                height: 1.4,
              ),
            ),
            SizedBox(height: 24),
            _ActionCard(
              icon: Icons.wifi,
              title: 'Configure it now',
              body:
                  "Join this lamp's own Wi-Fi network from your phone's "
                  "Wi-Fi settings (the SSID is the lamp's name). Once "
                  'connected, open this URL in any browser:',
              url: 'http://192.168.4.1',
              launch: false,
            ),
            SizedBox(height: 16),
            _ActionCard(
              icon: Icons.system_update_alt,
              title: 'Get app + mesh features',
              body:
                  'Flash the latest firmware. After the update finishes, '
                  'this lamp will rejoin the mesh and become fully '
                  'controllable from here.',
              url: 'https://update.lamplit.ca',
              launch: true,
            ),
          ],
        ),
      ),
    );
  }
}

/// One of the two action panels. When `launch: true` the URL tile is a
/// tap target that opens the system browser; when false it's a tap-to-
/// copy affordance (because the user needs to switch Wi-Fi networks
/// before the URL would resolve).
class _ActionCard extends StatelessWidget {
  const _ActionCard({
    required this.icon,
    required this.title,
    required this.body,
    required this.url,
    required this.launch,
  });

  final IconData icon;
  final String title;
  final String body;
  final String url;
  final bool launch;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: BrandColors.slateGrey.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(
          color: BrandColors.slateGrey.withValues(alpha: 0.3),
        ),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(icon, color: BrandColors.glowPink, size: 22),
              const SizedBox(width: 10),
              Expanded(
                child: Text(
                  title,
                  style: const TextStyle(
                    color: BrandColors.lampWhite,
                    fontSize: 15,
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Text(
            body,
            style: const TextStyle(
              color: BrandColors.fogGrey,
              fontSize: 13,
              height: 1.4,
            ),
          ),
          const SizedBox(height: 12),
          InkWell(
            onTap: () async {
              if (launch) {
                final uri = Uri.parse(url);
                final ok = await launchUrl(uri,
                    mode: LaunchMode.externalApplication);
                if (!ok) {
                  // Fallback: at least put the URL on the clipboard so the
                  // user can paste it into a browser manually.
                  await Clipboard.setData(ClipboardData(text: url));
                  if (context.mounted) {
                    ScaffoldMessenger.of(context).showSnackBar(
                      SnackBar(
                        duration: const Duration(seconds: 2),
                        content:
                            Text("Couldn't open — copied $url instead"),
                      ),
                    );
                  }
                }
              } else {
                await Clipboard.setData(ClipboardData(text: url));
                if (context.mounted) {
                  ScaffoldMessenger.of(context).showSnackBar(
                    SnackBar(
                      duration: const Duration(seconds: 2),
                      content: Text('Copied $url'),
                    ),
                  );
                }
              }
            },
            borderRadius: BorderRadius.circular(8),
            child: Container(
              padding: const EdgeInsets.symmetric(
                  horizontal: 10, vertical: 8),
              decoration: BoxDecoration(
                color: BrandColors.midnightBlack,
                borderRadius: BorderRadius.circular(8),
                border: Border.all(
                  color: BrandColors.glowPink.withValues(alpha: 0.5),
                ),
              ),
              child: Row(
                children: [
                  Expanded(
                    child: Text(
                      url,
                      style: const TextStyle(
                        color: BrandColors.glowPink,
                        fontSize: 13,
                        fontFamily: 'monospace',
                      ),
                    ),
                  ),
                  Icon(
                    launch ? Icons.open_in_new : Icons.copy,
                    color: BrandColors.slateGrey,
                    size: 16,
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}
