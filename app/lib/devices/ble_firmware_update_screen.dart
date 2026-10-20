import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:http/http.dart' as http;
import 'package:permission_handler/permission_handler.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

import '../main.dart';
import 'ble_pairing_screen.dart' show SynoraBleUuids;
import 'device_detail_screen.dart';

enum _Step {
  permissions,
  scanning,
  connecting,
  selectingBuild,
  downloading,
  uploading,
  done,
  error,
}

// Fallback for a device with no Wi-Fi yet (or that never got provisioned at
// all): pushes a firmware build already uploaded to the server straight over
// the same BLE provisioning connection used for Wi-Fi pairing, via the
// `firmware` characteristic added in BleProvisioning. Same
// scan/connect/discoverServices pattern as BlePairingScreen, trimmed to just
// the firmware-push steps.
class BleFirmwareUpdateScreen extends ConsumerStatefulWidget {
  const BleFirmwareUpdateScreen({super.key});

  @override
  ConsumerState<BleFirmwareUpdateScreen> createState() => _BleFirmwareUpdateScreenState();
}

class _BleFirmwareUpdateScreenState extends ConsumerState<BleFirmwareUpdateScreen> {
  _Step _step = _Step.permissions;
  BluetoothDevice? _device;
  String? _error;
  final Map<DeviceIdentifier, ScanResult> _discovered = {};
  StreamSubscription<List<ScanResult>>? _scanSub;
  bool _scanFinished = false;
  List<Map<String, dynamic>> _builds = [];
  double _uploadProgress = 0;
  String _statusText = '';

  @override
  void initState() {
    super.initState();
    // Same reasoning as BlePairingScreen: several steps here (scan, BLE
    // chunked transfer of a multi-MB binary) can run well past a minute, and
    // the screen locking mid-transfer would interrupt the BLE link.
    WakelockPlus.enable();
    _requestPermissionsThenScan();
  }

  @override
  void dispose() {
    WakelockPlus.disable();
    _scanSub?.cancel();
    FlutterBluePlus.stopScan();
    _device?.disconnect().catchError((_) {});
    super.dispose();
  }

  Future<void> _requestPermissionsThenScan() async {
    if (!mounted) return;
    setState(() => _step = _Step.permissions);
    final statuses = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();
    final bluetoothGranted =
        statuses[Permission.bluetoothScan]!.isGranted && statuses[Permission.bluetoothConnect]!.isGranted;
    if (!bluetoothGranted) {
      if (!mounted) return;
      setState(() {
        _step = _Step.error;
        _error = 'Cần có quyền Bluetooth để cập nhật firmware qua Bluetooth.';
      });
      return;
    }
    await _scan();
  }

  Future<void> _scan() async {
    if (!mounted) return;
    setState(() {
      _step = _Step.scanning;
      _scanFinished = false;
      _discovered.clear();
    });
    try {
      await _scanSub?.cancel();
      _scanSub = FlutterBluePlus.scanResults.listen((results) {
        if (!mounted) return;
        setState(() {
          for (final r in results) {
            if (r.device.platformName.isNotEmpty) _discovered[r.device.remoteId] = r;
          }
        });
      });
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 15));
      if (mounted) setState(() => _scanFinished = true);
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _step = _Step.error;
        _error = e.toString();
      });
    }
  }

  Future<void> _selectDevice(BluetoothDevice device) async {
    await FlutterBluePlus.stopScan();
    await _scanSub?.cancel();
    _device = device;
    await _connectThenLoadBuilds();
  }

  Future<void> _connectThenLoadBuilds() async {
    if (!mounted) return;
    setState(() => _step = _Step.connecting);
    try {
      await _device!.connect();
      // Negotiate a bigger ATT MTU -- NimBLE's 23-byte default (20-byte
      // payload) would make a multi-MB transfer impractically slow. Best
      // effort: if the peripheral doesn't grant it, writes just fall back to
      // smaller chunks sized off mtuNow. requestMtu is Android-only (throws
      // on iOS, which negotiates the MTU itself on connect).
      if (Platform.isAndroid) {
        try {
          await _device!.requestMtu(517);
        } catch (_) {}
      }
      await _device!.discoverServices();

      final builds = await ref.read(apiClientProvider).listFirmwareBuilds();
      if (!mounted) return;
      if (builds.isEmpty) {
        setState(() {
          _step = _Step.error;
          _error = 'Chưa có bản firmware nào trên máy chủ.';
        });
        return;
      }
      setState(() {
        _builds = builds;
        _step = _Step.selectingBuild;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _step = _Step.error;
        _error = e.toString();
      });
    }
  }

  Future<void> _pushBuild(Map<String, dynamic> build) async {
    if (!mounted) return;
    setState(() {
      _step = _Step.downloading;
      _uploadProgress = 0;
      _statusText = 'Đang tải firmware từ máy chủ…';
    });
    try {
      final baseUrl = ref.read(apiClientProvider).baseUrl;
      final buildId = build['id'] as String;
      final sha256 = build['sha256'] as String;
      final res = await http.get(Uri.parse('$baseUrl/firmware/$buildId/binary')).timeout(const Duration(seconds: 180));
      if (res.statusCode != 200) {
        throw Exception('Tải firmware thất bại (HTTP ${res.statusCode})');
      }
      final bytes = res.bodyBytes;
      final expectedSize = (build['sizeBytes'] as num).toInt();
      if (bytes.length != expectedSize) {
        throw Exception('Firmware tải về không đầy đủ (${bytes.length}/$expectedSize byte)');
      }
      if (!mounted) return;

      final services = await _device!.discoverServices();
      final service = services.firstWhere((s) => s.uuid == SynoraBleUuids.service);
      final chars = {for (final c in service.characteristics) c.uuid: c};
      final controlChar = chars[SynoraBleUuids.control]!;
      final statusChar = chars[SynoraBleUuids.status]!;
      final firmwareChar = chars[SynoraBleUuids.firmware]!;

      await statusChar.setNotifyValue(true);
      String? otaError;
      // Completed by either the BLE link dropping (success: device restarts
      // after commit) or an ota_error notification, whichever comes first.
      final disconnected = Completer<void>();
      final sub = statusChar.onValueReceived.map((b) => String.fromCharCodes(b)).listen((s) {
        if (s.startsWith('ota_error:')) {
          otaError = s.substring('ota_error:'.length);
          if (!disconnected.isCompleted) disconnected.completeError(Exception(otaError));
        } else if (s.startsWith('ota_progress:') && mounted) {
          final percent = int.tryParse(s.substring('ota_progress:'.length));
          if (percent != null) setState(() => _uploadProgress = percent / 100);
        }
      });
      // Avoid an unhandled-error report if ota_error fires before anything awaits the future.
      disconnected.future.ignore();

      try {
        if (!mounted) return;
        setState(() {
          _step = _Step.uploading;
          _statusText = 'Đang gửi firmware qua Bluetooth…';
        });
        await controlChar.write('ota_start:${bytes.length}:$sha256'.codeUnits, withoutResponse: false);

        // ATT payload is MTU minus a 3-byte header; mtuNow is whatever was
        // negotiated on connect.
        final chunkSize = (_device!.mtuNow - 3).clamp(20, 512);

        // No per-chunk delay needed: fbp's write() awaits the platform's
        // write callback even for withoutResponse, which provides backpressure.
        for (var offset = 0; offset < bytes.length; offset += chunkSize) {
          if (otaError != null) break;
          final end = (offset + chunkSize).clamp(0, bytes.length);
          await firmwareChar.write(bytes.sublist(offset, end), withoutResponse: true);
          if (mounted) setState(() => _uploadProgress = end / bytes.length);
        }

        if (otaError != null) {
          throw Exception(otaError);
        }

        if (!mounted) return;
        setState(() => _statusText = 'Đang xác minh và cài đặt…');
        await controlChar.write('ota_commit'.codeUnits, withoutResponse: false);

        final disconnectSub = _device!.connectionState.listen((state) {
          if (state == BluetoothConnectionState.disconnected && !disconnected.isCompleted) {
            disconnected.complete();
          }
        });
        try {
          await disconnected.future.timeout(const Duration(seconds: 30), onTimeout: () {});
        } finally {
          await disconnectSub.cancel();
        }

        if (otaError != null) throw Exception(otaError);
        if (!mounted) return;
        setState(() => _step = _Step.done);
      } finally {
        await sub.cancel();
      }
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _step = _Step.error;
        _error = e.toString();
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: DeviceDetailScreen.backgroundColor,
      appBar: AppBar(
        backgroundColor: DeviceDetailScreen.backgroundColor,
        foregroundColor: Colors.white,
        title: const Text('Cập nhật firmware qua Bluetooth'),
      ),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: DefaultTextStyle.merge(
          style: const TextStyle(color: Colors.white),
          child: switch (_step) {
            _Step.permissions => const Center(child: Text('Đang yêu cầu quyền…')),
            _Step.scanning => _buildScanningView(),
            _Step.connecting => const Center(child: Text('Đang kết nối…')),
            _Step.selectingBuild => _buildSelectBuildView(),
            _Step.downloading || _Step.uploading => Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    CircularProgressIndicator(
                      value: _step == _Step.uploading ? _uploadProgress : null,
                      color: DeviceDetailScreen.goodColor,
                    ),
                    const SizedBox(height: 16),
                    Text(_statusText, textAlign: TextAlign.center),
                    if (_step == _Step.uploading) ...[
                      const SizedBox(height: 8),
                      Text('${(_uploadProgress * 100).toStringAsFixed(0)}%',
                          style: const TextStyle(color: Colors.white70)),
                    ],
                  ],
                ),
              ),
            _Step.done => Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Icon(Icons.check_circle, color: DeviceDetailScreen.goodColor, size: 48),
                    const SizedBox(height: 16),
                    const Text('Đã cập nhật firmware. Thiết bị đang khởi động lại.'),
                    const SizedBox(height: 16),
                    FilledButton(
                      style: FilledButton.styleFrom(
                        backgroundColor: DeviceDetailScreen.goodColor,
                        foregroundColor: DeviceDetailScreen.backgroundColor,
                      ),
                      onPressed: () => Navigator.of(context).pop(),
                      child: const Text('Xong'),
                    ),
                  ],
                ),
              ),
            _Step.error => Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(_error ?? 'Đã xảy ra lỗi.',
                        textAlign: TextAlign.center, style: const TextStyle(color: DeviceDetailScreen.lowColor)),
                    const SizedBox(height: 16),
                    FilledButton(
                      style: FilledButton.styleFrom(
                        backgroundColor: DeviceDetailScreen.goodColor,
                        foregroundColor: DeviceDetailScreen.backgroundColor,
                      ),
                      onPressed: _requestPermissionsThenScan,
                      child: const Text('Thử lại'),
                    ),
                  ],
                ),
              ),
          },
        ),
      ),
    );
  }

  Widget _buildSelectBuildView() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        const Text('Chọn bản firmware để gửi qua Bluetooth:'),
        const SizedBox(height: 12),
        Expanded(
          child: ListView.builder(
            itemCount: _builds.length,
            itemBuilder: (context, index) {
              final build = _builds[index];
              return Container(
                margin: const EdgeInsets.only(bottom: 8),
                decoration:
                    BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(12)),
                child: ListTile(
                  shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                  title: Text(build['version'] as String, style: const TextStyle(color: Colors.white)),
                  subtitle: Text(
                    '${((build['sizeBytes'] as num) / 1024).toStringAsFixed(0)} KB'
                    '${build['notes'] != null ? ' · ${build['notes']}' : ''}',
                    style: const TextStyle(color: Colors.white70),
                  ),
                  trailing: const Icon(Icons.bluetooth, color: Colors.white70),
                  onTap: () => _pushBuild(build),
                ),
              );
            },
          ),
        ),
      ],
    );
  }

  Widget _buildScanningView() {
    bool isSynora(ScanResult r) => r.device.platformName.toLowerCase().contains('synora');
    final devices = _discovered.values.toList()
      ..sort((a, b) {
        final synoraCompare = (isSynora(b) ? 1 : 0).compareTo(isSynora(a) ? 1 : 0);
        return synoraCompare != 0 ? synoraCompare : b.rssi.compareTo(a.rssi);
      });

    return Column(
      children: [
        Row(
          children: [
            if (!_scanFinished) ...[
              const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2)),
              const SizedBox(width: 12),
              const Expanded(child: Text('Đang quét thiết bị lân cận…')),
            ] else ...[
              const Expanded(child: Text('Quét hoàn tất.')),
              IconButton(
                tooltip: 'Quét lại',
                icon: const Icon(Icons.refresh_rounded, color: Colors.white),
                onPressed: _scan,
              ),
            ],
          ],
        ),
        const SizedBox(height: 12),
        if (devices.isEmpty && _scanFinished)
          const Expanded(child: Center(child: Text('Không tìm thấy thiết bị Bluetooth nào gần đây.')))
        else
          Expanded(
            child: ListView.builder(
              itemCount: devices.length,
              itemBuilder: (context, index) {
                final result = devices[index];
                final synora = isSynora(result);
                return Container(
                  margin: const EdgeInsets.only(bottom: 8),
                  decoration:
                      BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(16)),
                  child: Opacity(
                    opacity: synora ? 1 : 0.5,
                    child: ListTile(
                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
                      leading: Icon(synora ? Icons.bluetooth_connected : Icons.bluetooth,
                          color: synora ? DeviceDetailScreen.goodColor : Colors.white70),
                      title: Text(result.device.platformName, style: const TextStyle(color: Colors.white)),
                      subtitle: Text('${result.device.remoteId} · RSSI ${result.rssi}',
                          style: const TextStyle(color: Colors.white70)),
                      trailing: synora
                          ? Chip(
                              label: const Text('Synora', style: TextStyle(color: Colors.white)),
                              backgroundColor: const Color(0xFF163826))
                          : null,
                      onTap: synora ? () => _selectDevice(result.device) : null,
                    ),
                  ),
                );
              },
            ),
          ),
      ],
    );
  }
}
