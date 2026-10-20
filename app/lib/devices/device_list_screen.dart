import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../auth/user_profile_screen.dart';
import '../main.dart';
import 'ble_firmware_update_screen.dart';
import 'ble_pairing_screen.dart';
import 'device_detail_screen.dart';

class DeviceListScreen extends ConsumerStatefulWidget {
  const DeviceListScreen({super.key});

  @override
  ConsumerState<DeviceListScreen> createState() => _DeviceListScreenState();
}

class _DeviceListScreenState extends ConsumerState<DeviceListScreen> with RouteAware {
  late Future<List<Map<String, dynamic>>> _devices;
  bool _isAdmin = false;
  String? _myAvatarUrl;

  @override
  void initState() {
    super.initState();
    // Plain field assignment, NOT _reload() -- this runs synchronously while
    // this widget is still being mounted as part of the parent's build (e.g.
    // right after login flips isLoggedInProvider), so calling setState() here
    // (as _reload() does) throws "setState() or markNeedsBuild() called
    // during build." The first build already picks up this field directly.
    _devices = ref.read(apiClientProvider).listDevices();
    // Owner-name chip is only meaningful for Admin/SuperAdmin, who see every
    // user's devices -- a regular user only ever sees their own, so it'd just
    // show their own username back at them.
    ref.read(apiClientProvider).currentUserRole().then((role) {
      if (mounted) setState(() => _isAdmin = role == 'Admin' || role == 'SuperAdmin');
    }).catchError((_) {});
    _loadMyAvatar();
  }

  Future<void> _loadMyAvatar() async {
    try {
      final profile = await ref.read(apiClientProvider).getMyProfile();
      if (mounted) setState(() => _myAvatarUrl = profile['avatarUrl'] as String?);
    } catch (_) {
      // Non-fatal -- the AppBar just shows the fallback person icon.
    }
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    routeObserver.subscribe(this, ModalRoute.of(context) as PageRoute);
  }

  @override
  void dispose() {
    routeObserver.unsubscribe(this);
    super.dispose();
  }

  // Fires whenever a route pushed on top of this one (BlePairingScreen,
  // DeviceDetailScreen, or anywhere else) gets popped and this screen is
  // visible again -- reloads regardless of how the caller got back here,
  // instead of relying on each push site to remember to trigger a reload.
  @override
  void didPopNext() => _reload();

  Future<void> _reload() {
    final future = ref.read(apiClientProvider).listDevices();
    // Statement body, not `=> _devices = future` -- an assignment expression
    // evaluates to the assigned value, so an arrow closure here would return
    // a Future, and setState() throws "callback argument returned a Future"
    // when that happens (this crashed live -- confirmed via logcat).
    setState(() {
      _devices = future;
    });
    return future;
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: DeviceDetailScreen.backgroundColor,
      appBar: AppBar(
        backgroundColor: DeviceDetailScreen.backgroundColor,
        foregroundColor: Colors.white,
        title: const Text('Thiết bị'),
        actions: [
          IconButton(
            icon: const Icon(Icons.add),
            tooltip: 'Ghép nối thiết bị',
            onPressed: () {
              Navigator.of(context).push(MaterialPageRoute(builder: (_) => BlePairingScreen(onPaired: _reload)));
            },
          ),
          // Fallback update path for a device with no Wi-Fi (or not yet
          // provisioned) -- pushes a server-side build over BLE instead of
          // the normal Wi-Fi OTA triggered from DeviceDetailScreen. Same
          // admin gate as that screen's firmware button (GET /admin/firmware
          // is admin-only).
          if (_isAdmin)
            IconButton(
              icon: const Icon(Icons.bluetooth_searching),
              tooltip: 'Cập nhật firmware qua Bluetooth',
              onPressed: () {
                Navigator.of(context).push(MaterialPageRoute(builder: (_) => const BleFirmwareUpdateScreen()));
              },
            ),
          IconButton(
            tooltip: 'Tài khoản',
            icon: CircleAvatar(
              radius: 14,
              backgroundColor: const Color(0xFF163826),
              backgroundImage:
                  _myAvatarUrl != null ? NetworkImage('${ref.read(apiClientProvider).baseUrl}$_myAvatarUrl') : null,
              child: _myAvatarUrl == null ? const Icon(Icons.person, size: 16, color: Colors.white70) : null,
            ),
            onPressed: () async {
              await Navigator.of(context).push(MaterialPageRoute(builder: (_) => const UserProfileScreen()));
              _loadMyAvatar();
            },
          ),
        ],
      ),
      body: RefreshIndicator(
        onRefresh: _reload,
        color: DeviceDetailScreen.goodColor,
        backgroundColor: DeviceDetailScreen.cardColor,
        child: FutureBuilder<List<Map<String, dynamic>>>(
          future: _devices,
          builder: (context, snapshot) {
            // hasError must be checked explicitly -- otherwise a thrown
            // exception (timeout, expired token, network hiccup) leaves
            // hasData false forever, showing a permanent, undismissable
            // loading spinner that looks exactly like the app froze.
            if (snapshot.hasError) {
              return ListView(
                physics: const AlwaysScrollableScrollPhysics(),
                children: [
                  const SizedBox(height: 120),
                  Center(
                    child: Text('Không thể tải danh sách thiết bị: ${snapshot.error}',
                        style: const TextStyle(color: Colors.white70)),
                  ),
                  const SizedBox(height: 16),
                  Center(child: FilledButton(onPressed: _reload, child: const Text('Thử lại'))),
                ],
              );
            }
            if (!snapshot.hasData) {
              return const Center(child: CircularProgressIndicator(color: DeviceDetailScreen.goodColor));
            }
            final devices = snapshot.data!;
            if (devices.isEmpty) {
              return ListView(
                physics: const AlwaysScrollableScrollPhysics(),
                children: const [
                  SizedBox(height: 120),
                  Center(
                    child: Text('Chưa có thiết bị nào được ghép nối.', style: TextStyle(color: Colors.white70)),
                  ),
                ],
              );
            }
            return ListView(
              physics: const AlwaysScrollableScrollPhysics(),
              children: devices.map((d) => _DeviceCard(device: d, isAdmin: _isAdmin, onChanged: _reload)).toList(),
            );
          },
        ),
      ),
    );
  }
}

class _DeviceCard extends ConsumerWidget {
  const _DeviceCard({required this.device, required this.isAdmin, required this.onChanged});

  final Map<String, dynamic> device;
  final bool isAdmin;
  final VoidCallback onChanged;

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final owner = device['owner'] as String?;
    final avatarUrl = device['avatarUrl'] as String?;
    final reading = device['latestReading'] as Map<String, dynamic>?;
    final baseUrl = ref.read(apiClientProvider).baseUrl;
    final warningThreshold = device['lowWaterWarningThresholdPercent'] as int?;
    final waterLevel = (reading?['waterLevelPercent'] as num?)?.toDouble();
    final isWaterWarning = warningThreshold != null && waterLevel != null && waterLevel <= warningThreshold;
    final isOnline = device['isOnline'] as bool? ?? false;

    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      decoration: BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(20)),
      child: Material(
        color: Colors.transparent,
        borderRadius: BorderRadius.circular(20),
        child: InkWell(
          borderRadius: BorderRadius.circular(20),
          onTap: () async {
            await Navigator.of(context)
                .push(MaterialPageRoute(builder: (_) => DeviceDetailScreen(device: device)));
            onChanged();
          },
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Stack(
                      clipBehavior: Clip.none,
                      children: [
                        CircleAvatar(
                          radius: 20,
                          backgroundColor: const Color(0xFF163826),
                          backgroundImage: avatarUrl != null ? NetworkImage('$baseUrl$avatarUrl') : null,
                          child: avatarUrl == null
                              ? const Icon(Icons.local_florist, color: DeviceDetailScreen.goodColor)
                              : null,
                        ),
                        Positioned(
                          right: -2,
                          bottom: -2,
                          child: Container(
                            width: 12,
                            height: 12,
                            decoration: BoxDecoration(
                              shape: BoxShape.circle,
                              color: isOnline ? DeviceDetailScreen.goodColor : Colors.white38,
                              border: Border.all(color: DeviceDetailScreen.cardColor, width: 2),
                            ),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(device['name'] as String,
                          style: const TextStyle(color: Colors.white, fontWeight: FontWeight.bold, fontSize: 16)),
                    ),
                    if (isAdmin && owner != null)
                      Chip(
                        label: Text(owner, style: const TextStyle(color: Colors.white)),
                        backgroundColor: const Color(0xFF163826),
                      ),
                  ],
                ),
                const SizedBox(height: 8),
                if (reading == null)
                  const Text('Chưa có dữ liệu cảm biến.', style: TextStyle(color: Colors.white70, fontSize: 12))
                else
                  Wrap(
                    spacing: 16,
                    runSpacing: 4,
                    children: [
                      _Reading('Nhiệt độ', reading['temperatureCelsius'], '°C'),
                      _Reading('Độ ẩm không khí', reading['humidityPercent'], '%'),
                      _Reading('Độ ẩm đất', reading['soilMoisturePercent'], '%'),
                      _Reading('Mực nước', reading['waterLevelPercent'], '%', isWarning: isWaterWarning),
                    ],
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _Reading extends StatelessWidget {
  const _Reading(this.label, this.value, this.unit, {this.isWarning = false});

  final String label;
  final num? value;
  final String unit;
  final bool isWarning;

  static const _warningColor = Color(0xFFFFC107);

  @override
  Widget build(BuildContext context) {
    final content = Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          children: [
            Text(label, style: const TextStyle(color: Colors.white70, fontSize: 11)),
            if (isWarning) ...[
              const SizedBox(width: 4),
              const Icon(Icons.warning_amber_rounded, color: _warningColor, size: 12),
            ],
          ],
        ),
        Text(value != null ? '${value!.toStringAsFixed(0)}$unit' : '—',
            style: TextStyle(color: isWarning ? _warningColor : Colors.white, fontWeight: FontWeight.w600)),
      ],
    );
    if (!isWarning) return content;
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      decoration: BoxDecoration(
        color: _warningColor.withValues(alpha: 0.18),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: _warningColor, width: 1),
      ),
      child: content,
    );
  }
}
