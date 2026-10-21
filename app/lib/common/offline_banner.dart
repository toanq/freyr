import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'network_status.dart';

const offlineBannerText = 'Không có kết nối mạng — kiểm tra Wi-Fi hoặc dữ liệu di động';

// Wraps the whole app (MaterialApp.builder) so the banner shows above every
// screen. It takes over the top safe-area inset while visible and removes it
// from the page below, so the page's own AppBar doesn't pad the status bar a
// second time.
class OfflineBannerScope extends ConsumerWidget {
  const OfflineBannerScope({super.key, required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final online = ref.watch(networkStatusProvider);
    if (online) return child;
    return Column(
      children: [
        Material(
          color: const Color(0xFF8A3B12),
          child: SafeArea(
            bottom: false,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              child: Row(
                children: const [
                  Icon(Icons.wifi_off_rounded, color: Colors.white, size: 18),
                  SizedBox(width: 8),
                  Expanded(
                    child: Text(offlineBannerText, style: TextStyle(color: Colors.white, fontSize: 13)),
                  ),
                ],
              ),
            ),
          ),
        ),
        Expanded(
          child: MediaQuery.removePadding(context: context, removeTop: true, child: child),
        ),
      ],
    );
  }
}

// For actions that can't work offline: shows the message and returns false
// instead of letting the screen fire a request that can only fail.
bool ensureOnline(BuildContext context, WidgetRef ref) {
  if (ref.read(networkStatusProvider)) return true;
  ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Không có kết nối mạng')));
  return false;
}
