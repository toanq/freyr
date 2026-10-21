import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import 'device_detail_screen.dart';

// Shown instead of a BLE flow's normal content while the adapter isn't usable.
// Platform comes from Theme (not dart:io) so widget tests can override it.
class BluetoothStateCard extends StatelessWidget {
  const BluetoothStateCard({
    super.key,
    required this.state,
    this.onTurnOn,
    this.onOpenSettings,
  });

  final BluetoothAdapterState state;
  final VoidCallback? onTurnOn;
  final VoidCallback? onOpenSettings;

  @override
  Widget build(BuildContext context) {
    final isIos = Theme.of(context).platform == TargetPlatform.iOS;
    final (icon, title, body) = switch (state) {
      BluetoothAdapterState.unavailable => (
          Icons.bluetooth_disabled_rounded,
          'Không hỗ trợ Bluetooth',
          'Điện thoại này không có Bluetooth nên không thể ghép nối hoặc cập nhật thiết bị qua Bluetooth.',
        ),
      BluetoothAdapterState.unauthorized => (
          Icons.bluetooth_disabled_rounded,
          'Chưa cấp quyền Bluetooth',
          'Hãy cho phép Synora dùng Bluetooth trong Cài đặt, rồi quay lại.',
        ),
      BluetoothAdapterState.turningOn || BluetoothAdapterState.turningOff || BluetoothAdapterState.unknown => (
          Icons.bluetooth_searching_rounded,
          'Đang kiểm tra Bluetooth…',
          '',
        ),
      _ => (
          Icons.bluetooth_disabled_rounded,
          'Bluetooth đang tắt',
          isIos
              ? 'Hãy bật Bluetooth trong Trung tâm điều khiển hoặc Cài đặt > Bluetooth. Ứng dụng sẽ tự tiếp tục khi Bluetooth bật.'
              : 'Cần bật Bluetooth để tìm và kết nối thiết bị. Ứng dụng sẽ tự tiếp tục khi Bluetooth bật.',
        ),
    };
    final isOff = state == BluetoothAdapterState.off;
    final busy = state == BluetoothAdapterState.turningOn ||
        state == BluetoothAdapterState.turningOff ||
        state == BluetoothAdapterState.unknown;

    return Center(
      child: Container(
        padding: const EdgeInsets.all(20),
        decoration: BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(20)),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            if (busy)
              const SizedBox(width: 32, height: 32, child: CircularProgressIndicator(strokeWidth: 3))
            else
              Icon(icon, size: 40, color: DeviceDetailScreen.lowColor),
            const SizedBox(height: 12),
            Text(title,
                textAlign: TextAlign.center,
                style: const TextStyle(color: Colors.white, fontSize: 17, fontWeight: FontWeight.w600)),
            if (body.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(body, textAlign: TextAlign.center, style: const TextStyle(color: Colors.white70)),
            ],
            const SizedBox(height: 16),
            if (isOff && !isIos && onTurnOn != null)
              FilledButton.icon(
                style: FilledButton.styleFrom(
                  backgroundColor: DeviceDetailScreen.goodColor,
                  foregroundColor: DeviceDetailScreen.backgroundColor,
                ),
                onPressed: onTurnOn,
                icon: const Icon(Icons.bluetooth_rounded),
                label: const Text('Bật Bluetooth'),
              ),
            if ((state == BluetoothAdapterState.unauthorized || (isOff && isIos)) && onOpenSettings != null)
              OutlinedButton(
                onPressed: onOpenSettings,
                child: const Text('Mở cài đặt', style: TextStyle(color: Colors.white)),
              ),
          ],
        ),
      ),
    );
  }
}

// BLE scanning on Android 11 and older also requires Location Services.
class LocationServicesCard extends StatelessWidget {
  const LocationServicesCard({super.key, required this.onRetry});

  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Container(
        padding: const EdgeInsets.all(20),
        decoration: BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(20)),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Icon(Icons.location_off_rounded, size: 40, color: DeviceDetailScreen.lowColor),
            const SizedBox(height: 12),
            const Text('Vị trí đang tắt',
                textAlign: TextAlign.center,
                style: TextStyle(color: Colors.white, fontSize: 17, fontWeight: FontWeight.w600)),
            const SizedBox(height: 8),
            const Text(
              'Trên phiên bản Android này, cần bật Vị trí (GPS) để quét thiết bị Bluetooth. '
              'Hãy bật Vị trí trong thanh cài đặt nhanh rồi thử lại.',
              textAlign: TextAlign.center,
              style: TextStyle(color: Colors.white70),
            ),
            const SizedBox(height: 16),
            FilledButton(
              style: FilledButton.styleFrom(
                backgroundColor: DeviceDetailScreen.goodColor,
                foregroundColor: DeviceDetailScreen.backgroundColor,
              ),
              onPressed: onRetry,
              child: const Text('Thử lại'),
            ),
          ],
        ),
      ),
    );
  }
}

// "Bluetooth đã bị tắt" after an interrupted flow. Retry is only enabled once
// the adapter is back on.
class BluetoothInterruptedView extends StatelessWidget {
  const BluetoothInterruptedView({
    super.key,
    required this.message,
    required this.bluetoothOn,
    required this.onRetry,
    this.onTurnOn,
  });

  final String message;
  final bool bluetoothOn;
  final VoidCallback onRetry;
  final VoidCallback? onTurnOn;

  @override
  Widget build(BuildContext context) {
    final isIos = Theme.of(context).platform == TargetPlatform.iOS;
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Icon(Icons.bluetooth_disabled_rounded, size: 40, color: DeviceDetailScreen.lowColor),
          const SizedBox(height: 12),
          const Text('Bluetooth đã bị tắt',
              style: TextStyle(color: Colors.white, fontSize: 17, fontWeight: FontWeight.w600)),
          const SizedBox(height: 8),
          Text(message, textAlign: TextAlign.center, style: const TextStyle(color: Colors.white70)),
          const SizedBox(height: 16),
          if (!bluetoothOn && !isIos && onTurnOn != null) ...[
            OutlinedButton(
              onPressed: onTurnOn,
              child: const Text('Bật Bluetooth', style: TextStyle(color: Colors.white)),
            ),
            const SizedBox(height: 8),
          ],
          FilledButton(
            style: FilledButton.styleFrom(
              backgroundColor: DeviceDetailScreen.goodColor,
              foregroundColor: DeviceDetailScreen.backgroundColor,
            ),
            onPressed: bluetoothOn ? onRetry : null,
            child: Text(bluetoothOn ? 'Thử lại' : 'Chờ bật Bluetooth…'),
          ),
        ],
      ),
    );
  }
}
