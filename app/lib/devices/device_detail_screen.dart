import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:image_picker/image_picker.dart';

import '../common/network_errors.dart';
import '../common/network_status.dart';
import '../common/offline_banner.dart';
import '../main.dart';
import 'assistant_talk_sheet.dart';

// Per-device dashboard styled after the reference mockup (dark green theme,
// hero plant graphic, metric tile grid, AI care suggestion). Deliberately
// scoped to this screen only -- the rest of the app keeps its existing
// light Material 3 theme.
class DeviceDetailScreen extends ConsumerStatefulWidget {
  const DeviceDetailScreen({super.key, required this.device});

  final Map<String, dynamic> device;

  static const backgroundColor = Color(0xFF0E2418);
  static const cardColor = Color(0xFF15301F);
  static const goodColor = Color(0xFF6FCF97);
  static const lowColor = Color(0xFFEF9A6B);
  static const highColor = Color(0xFF66BFFF);

  @override
  ConsumerState<DeviceDetailScreen> createState() => _DeviceDetailScreenState();
}

class _DeviceDetailScreenState extends ConsumerState<DeviceDetailScreen> {
  late Map<String, dynamic> _device = widget.device;
  bool _uploadingAvatar = false;
  bool _wateringNow = false;
  bool _pinging = false;
  bool _togglingMuted = false;
  bool _isAdmin = false;
  List<Map<String, dynamic>> _schedules = [];

  String get _name => _device['name'] as String? ?? 'Cây của bạn';
  String? get _avatarUrl => _device['avatarUrl'] as String?;
  bool get _isOnline => _device['isOnline'] as bool? ?? false;
  String get _deviceId => _device['id'] as String;
  int? get _lowWaterWarning => _device['lowWaterWarningThresholdPercent'] as int?;
  int? get _lowWaterCritical => _device['lowWaterCriticalThresholdPercent'] as int?;
  int? get _lowWaterIntervalMinutes => _device['lowWaterAlertIntervalMinutes'] as int?;
  bool get _muted => _device['muted'] as bool? ?? false;
  String? get _firmwareVersion => _device['firmwareVersion'] as String?;

  @override
  void initState() {
    super.initState();
    _reload();
    // Back online after being offline: refresh readings/schedules.
    ref.listenManual<bool>(networkStatusProvider, (wasOnline, online) {
      if (wasOnline == false && online) _reload();
    });
    // Only admins can list firmware builds (GET /admin/firmware) -- gates the
    // firmware-update button so regular owners don't hit a dead-end action.
    ref.read(apiClientProvider).currentUserRole().then((role) {
      if (mounted) setState(() => _isAdmin = role == 'Admin' || role == 'SuperAdmin');
    }).catchError((_) {});
  }

  Future<void> _reload() async {
    // widget.device is a snapshot from whenever the list was last loaded --
    // refresh with the latest reading/name/avatar/thresholds every time this
    // screen opens (initState) or the user pulls to refresh.
    await Future.wait([
      ref.read(apiClientProvider).getDevice(_deviceId).then((fresh) {
        if (mounted) setState(() => _device = fresh);
      }).catchError((_) {}),
      _loadSchedules(),
    ]);
  }

  Future<void> _loadSchedules() async {
    try {
      final schedules = await ref.read(apiClientProvider).listWateringSchedules(_deviceId);
      if (mounted) setState(() => _schedules = schedules);
    } catch (_) {
      // Non-fatal -- the section just shows empty until the next reload.
    }
  }

  Future<void> _triggerManualWatering() async {
    if (!ensureOnline(context, ref)) return;
    final duration = await _promptDurationSeconds(defaultSeconds: 10);
    if (duration == null) return;

    setState(() => _wateringNow = true);
    try {
      await ref.read(apiClientProvider).triggerManualWatering(_deviceId, duration);
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Đã gửi yêu cầu tưới nước, thiết bị sẽ tưới trong giây lát.')),
        );
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể gửi yêu cầu tưới: ${friendlyError(e)}')));
      }
    } finally {
      if (mounted) setState(() => _wateringNow = false);
    }
  }

  Future<void> _pingDevice() async {
    if (!ensureOnline(context, ref)) return;
    setState(() => _pinging = true);
    try {
      await ref.read(apiClientProvider).pingDevice(_deviceId);
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Đã gửi ping, thiết bị sẽ phát âm thanh trong giây lát.')),
        );
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể gửi ping: ${friendlyError(e)}')));
      }
    } finally {
      if (mounted) setState(() => _pinging = false);
    }
  }

  Future<void> _toggleMuted() async {
    if (!ensureOnline(context, ref)) return;
    final newValue = !_muted;
    setState(() => _togglingMuted = true);
    try {
      await ref.read(apiClientProvider).setDeviceMuted(_deviceId, newValue);
      if (mounted) setState(() => _device = {..._device, 'muted': newValue});
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể cập nhật trạng thái tắt tiếng: ${friendlyError(e)}')));
      }
    } finally {
      if (mounted) setState(() => _togglingMuted = false);
    }
  }

  Future<void> _openAssistantTalk() async {
    if (!ensureOnline(context, ref)) return;
    await showModalBottomSheet<void>(
      context: context,
      isScrollControlled: true,
      backgroundColor: Colors.transparent,
      builder: (_) => AssistantTalkSheet(deviceId: _deviceId),
    );
  }

  // Common watering times -- shown as quick picks so adding a schedule
  // usually doesn't need the time picker at all.
  static const _presetTimes = [
    TimeOfDay(hour: 6, minute: 0),
    TimeOfDay(hour: 7, minute: 0),
    TimeOfDay(hour: 12, minute: 0),
    TimeOfDay(hour: 18, minute: 0),
    TimeOfDay(hour: 20, minute: 0),
  ];

  static String _formatTimeOfDay(TimeOfDay t) =>
      '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';

  Future<void> _addSchedule() async {
    if (!ensureOnline(context, ref)) return;
    final time = await _pickScheduleTime();
    if (time == null || !mounted) return;

    final duration = await _promptDurationSeconds(defaultSeconds: 10);
    if (duration == null) return;

    try {
      await ref
          .read(apiClientProvider)
          .createWateringSchedule(_deviceId, hour: time.hour, minute: time.minute, durationSeconds: duration);
      _loadSchedules();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể thêm lịch tưới: ${friendlyError(e)}')));
      }
    }
  }

  Future<TimeOfDay?> _pickScheduleTime({TimeOfDay? initialTime}) {
    return showModalBottomSheet<TimeOfDay>(
      context: context,
      backgroundColor: DeviceDetailScreen.cardColor,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(20))),
      builder: (sheetContext) {
        return SafeArea(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Padding(
                padding: EdgeInsets.fromLTRB(20, 20, 20, 8),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text('Chọn giờ tưới',
                      style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold, fontSize: 16)),
                ),
              ),
              for (final t in _presetTimes)
                ListTile(
                  leading: const Icon(Icons.schedule, color: Colors.white70),
                  title: Text(_formatTimeOfDay(t), style: const TextStyle(color: Colors.white)),
                  onTap: () => Navigator.of(sheetContext).pop(t),
                ),
              ListTile(
                leading: const Icon(Icons.edit_calendar, color: Colors.white70),
                title: const Text('Tùy chỉnh...', style: TextStyle(color: Colors.white)),
                onTap: () async {
                  // dialOnly -- Flutter's TimePickerEntryMode.input (the
                  // keyboard toggle) throws "BoxConstraints has
                  // non-normalized height constraints" on this Flutter SDK
                  // (confirmed live), so this hides that toggle entirely
                  // rather than letting the user hit a guaranteed crash.
                  // The dial itself covers the same functionality.
                  final picked = await showTimePicker(
                    context: sheetContext,
                    initialTime: initialTime ?? TimeOfDay.now(),
                    initialEntryMode: TimePickerEntryMode.dialOnly,
                  );
                  if (sheetContext.mounted) Navigator.of(sheetContext).pop(picked);
                },
              ),
            ],
          ),
        );
      },
    );
  }

  Future<void> _toggleSchedule(Map<String, dynamic> schedule, bool enabled) async {
    if (!ensureOnline(context, ref)) return;
    try {
      await ref.read(apiClientProvider).updateWateringSchedule(
            _deviceId,
            schedule['id'] as String,
            hour: schedule['hour'] as int,
            minute: schedule['minute'] as int,
            durationSeconds: schedule['durationSeconds'] as int,
            enabled: enabled,
          );
      _loadSchedules();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể cập nhật lịch tưới: ${friendlyError(e)}')));
      }
    }
  }

  Future<void> _editSchedule(Map<String, dynamic> schedule) async {
    if (!ensureOnline(context, ref)) return;
    final currentTime = TimeOfDay(hour: schedule['hour'] as int, minute: schedule['minute'] as int);
    final time = await _pickScheduleTime(initialTime: currentTime);
    if (time == null || !mounted) return;

    final duration = await _promptDurationSeconds(defaultSeconds: schedule['durationSeconds'] as int);
    if (duration == null) return;

    try {
      await ref.read(apiClientProvider).updateWateringSchedule(
            _deviceId,
            schedule['id'] as String,
            hour: time.hour,
            minute: time.minute,
            durationSeconds: duration,
            enabled: schedule['enabled'] as bool,
          );
      _loadSchedules();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể cập nhật lịch tưới: ${friendlyError(e)}')));
      }
    }
  }

  Future<void> _deleteSchedule(String scheduleId) async {
    if (!ensureOnline(context, ref)) return;
    try {
      await ref.read(apiClientProvider).deleteWateringSchedule(_deviceId, scheduleId);
      _loadSchedules();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể xóa lịch tưới: ${friendlyError(e)}')));
      }
    }
  }

  String _intervalLabel() {
    final minutes = _lowWaterIntervalMinutes;
    if (minutes == null) return '';
    if (minutes % 60 == 0) return ', nhắc lại mỗi ${minutes ~/ 60}h';
    return ', nhắc lại mỗi ${minutes}p';
  }

  static const _intervalPresets = [
    (label: '1 giờ', minutes: 60),
    (label: '5 giờ', minutes: 300),
    (label: '10 giờ', minutes: 600),
    (label: '24 giờ', minutes: 1440),
  ];

  Future<void> _editLowWaterAlerts() async {
    if (!ensureOnline(context, ref)) return;
    final config = await _promptLowWaterAlertConfig();
    if (config == null) return;
    final (warning, critical, intervalMinutes) = config;
    try {
      await ref.read(apiClientProvider).updateLowWaterAlerts(
            _deviceId,
            warningPercent: warning,
            criticalPercent: critical,
            intervalMinutes: intervalMinutes,
          );
      if (mounted) {
        setState(() => _device = {
              ..._device,
              'lowWaterWarningThresholdPercent': warning,
              'lowWaterCriticalThresholdPercent': critical,
              'lowWaterAlertIntervalMinutes': intervalMinutes,
            });
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể cập nhật cảnh báo: ${friendlyError(e)}')));
      }
    }
  }

  // Returns (warningPercent?, criticalPercent?, intervalMinutes?) -- either
  // percent can be null (0 in the fields below) to disable that tier.
  Future<(int?, int?, int?)?> _promptLowWaterAlertConfig() async {
    final warningController = TextEditingController(text: (_lowWaterWarning ?? 0).toString());
    final criticalController = TextEditingController(text: (_lowWaterCritical ?? 0).toString());
    final defaultIntervalMinutes = _lowWaterIntervalMinutes;
    final isCustomPreset =
        defaultIntervalMinutes != null && !_intervalPresets.any((p) => p.minutes == defaultIntervalMinutes);
    int? selectedInterval = defaultIntervalMinutes;
    bool showCustom = isCustomPreset;
    final customHoursController =
        TextEditingController(text: isCustomPreset ? (defaultIntervalMinutes / 60).toStringAsFixed(0) : '');
    String? error;

    final result = await showDialog<(int?, int?, int?)>(
      context: context,
      builder: (context) => StatefulBuilder(
        builder: (context, setDialogState) => AlertDialog(
          title: const Text('Cảnh báo mực nước thấp'),
          content: SingleChildScrollView(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                TextField(
                  controller: criticalController,
                  keyboardType: TextInputType.number,
                  inputFormatters: [_PercentInputFormatter()],
                  // Critical comes first and drives warning's valid range --
                  // warning must never sit below critical (the more severe,
                  // lower level), so raise it live the moment critical is
                  // typed above it instead of only fixing it at save.
                  onChanged: (text) {
                    final critical = int.tryParse(text) ?? 0;
                    final warning = int.tryParse(warningController.text) ?? 0;
                    if (critical != 0 && warning < critical) {
                      setDialogState(() => warningController.text = critical.toString());
                    } else {
                      // Still needed even when warning doesn't change value --
                      // warning's inputFormatter reads criticalController.text
                      // live, so its min bound must be recomputed either way.
                      setDialogState(() {});
                    }
                  },
                  decoration: const InputDecoration(
                    labelText: 'Ngưỡng khẩn cấp (gửi thông báo)',
                    suffixText: '%',
                    helperText: '0 = tắt',
                  ),
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: warningController,
                  keyboardType: TextInputType.number,
                  // Bounded to [critical, 100] once critical is set (nonzero)
                  // -- only free to go as low as 0 while critical is disabled.
                  inputFormatters: [
                    _PercentInputFormatter(minValue: int.tryParse(criticalController.text) ?? 0),
                  ],
                  decoration: InputDecoration(
                    labelText: 'Ngưỡng cảnh báo (hiện trong app)',
                    suffixText: '%',
                    helperText: (int.tryParse(criticalController.text) ?? 0) == 0
                        ? '0 = tắt'
                        : 'Tối thiểu bằng ngưỡng khẩn cấp',
                  ),
                ),
                const SizedBox(height: 16),
                const Text('Tần suất nhắc lại thông báo'),
                const SizedBox(height: 8),
                Wrap(
                  spacing: 8,
                  runSpacing: 4,
                  children: [
                    ChoiceChip(
                      label: const Text('Một lần'),
                      selected: !showCustom && selectedInterval == null,
                      onSelected: (_) => setDialogState(() {
                        showCustom = false;
                        selectedInterval = null;
                      }),
                    ),
                    for (final preset in _intervalPresets)
                      ChoiceChip(
                        label: Text(preset.label),
                        selected: !showCustom && selectedInterval == preset.minutes,
                        onSelected: (_) => setDialogState(() {
                          showCustom = false;
                          selectedInterval = preset.minutes;
                        }),
                      ),
                    ChoiceChip(
                      label: const Text('Tùy chỉnh'),
                      selected: showCustom,
                      onSelected: (_) => setDialogState(() => showCustom = true),
                    ),
                  ],
                ),
                if (showCustom) ...[
                  const SizedBox(height: 12),
                  TextField(
                    controller: customHoursController,
                    autofocus: true,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(labelText: 'Nhắc lại mỗi', suffixText: 'giờ'),
                  ),
                ],
                if (error != null) ...[
                  const SizedBox(height: 12),
                  Text(error!, style: TextStyle(color: Theme.of(context).colorScheme.error)),
                ],
              ],
            ),
          ),
          actions: [
            TextButton(onPressed: () => Navigator.of(context).pop(), child: const Text('Hủy')),
            FilledButton(
              onPressed: () {
                var warning = int.tryParse(warningController.text.trim()) ?? 0;
                final critical = int.tryParse(criticalController.text.trim()) ?? 0;
                if (warning < 0 || warning > 100 || critical < 0 || critical > 100) {
                  setDialogState(() => error = 'Giá trị phải trong khoảng 0-100.');
                  return;
                }
                // Auto-raise warning to match critical rather than blocking
                // save with an error -- warning must never sit below
                // critical (the more severe, lower level).
                if (warning != 0 && critical != 0 && warning < critical) {
                  warning = critical;
                }

                int? intervalMinutes;
                if (critical != 0) {
                  if (showCustom) {
                    final hours = int.tryParse(customHoursController.text.trim());
                    if (hours == null || hours < 1) {
                      setDialogState(() => error = 'Nhập số giờ hợp lệ.');
                      return;
                    }
                    intervalMinutes = hours * 60;
                  } else {
                    intervalMinutes = selectedInterval;
                  }
                }

                Navigator.of(context).pop((warning == 0 ? null : warning, critical == 0 ? null : critical, intervalMinutes));
              },
              child: const Text('Lưu'),
            ),
          ],
        ),
      ),
    );
    return result;
  }

  Future<int?> _promptDurationSeconds({required int defaultSeconds}) async {
    final controller = TextEditingController(text: defaultSeconds.toString());
    final result = await showDialog<int>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Thời gian tưới (giây)'),
        content: TextField(
          controller: controller,
          autofocus: true,
          keyboardType: TextInputType.number,
          decoration: const InputDecoration(suffixText: 'giây'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(), child: const Text('Hủy')),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(int.tryParse(controller.text.trim())),
            child: const Text('Lưu'),
          ),
        ],
      ),
    );
    if (result == null || result <= 0 || result > 300) return null;
    return result;
  }

  Future<void> _pickAndUploadAvatar() async {
    if (!ensureOnline(context, ref)) return;
    final source = await showModalBottomSheet<ImageSource>(
      context: context,
      builder: (context) => SafeArea(
        child: Wrap(
          children: [
            ListTile(
              leading: const Icon(Icons.camera_alt),
              title: const Text('Chụp ảnh'),
              onTap: () => Navigator.of(context).pop(ImageSource.camera),
            ),
            ListTile(
              leading: const Icon(Icons.photo_library),
              title: const Text('Chọn từ thư viện'),
              onTap: () => Navigator.of(context).pop(ImageSource.gallery),
            ),
          ],
        ),
      ),
    );
    if (source == null) return;

    final picked = await ImagePicker().pickImage(source: source, maxWidth: 1024, imageQuality: 85);
    if (picked == null) return;

    setState(() => _uploadingAvatar = true);
    try {
      final avatarUrl =
          await ref.read(apiClientProvider).uploadDeviceAvatar(_device['id'] as String, File(picked.path));
      if (mounted) {
        // avatarUrl is the same path every time ("/devices/{id}/avatar"), so
        // NetworkImage's cache (keyed by URL) would keep showing the old
        // bytes forever without this -- the upload itself already succeeds
        // server-side, this is purely a client-side stale-cache issue.
        final baseUrl = ref.read(apiClientProvider).baseUrl;
        PaintingBinding.instance.imageCache.evict(NetworkImage('$baseUrl$avatarUrl'));
        setState(() => _device = {..._device, 'avatarUrl': avatarUrl});
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể tải ảnh lên: ${friendlyError(e)}')));
      }
    } finally {
      if (mounted) setState(() => _uploadingAvatar = false);
    }
  }

  Future<void> _renameDevice() async {
    if (!ensureOnline(context, ref)) return;
    final controller = TextEditingController(text: _name);
    final newName = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Đổi tên thiết bị'),
        content: TextField(controller: controller, autofocus: true),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(), child: const Text('Hủy')),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(controller.text.trim()),
            child: const Text('Lưu'),
          ),
        ],
      ),
    );
    if (newName == null || newName.isEmpty || newName == _name) return;

    try {
      await ref.read(apiClientProvider).renameDevice(_device['id'] as String, newName);
      if (mounted) setState(() => _device = {..._device, 'name': newName});
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể đổi tên: ${friendlyError(e)}')));
      }
    }
  }

  Future<void> _deleteDevice() async {
    if (!ensureOnline(context, ref)) return;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Xóa thiết bị?'),
        content: Text('$_name sẽ bị xóa khỏi tài khoản của bạn.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Hủy')),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: Colors.red),
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('Xóa'),
          ),
        ],
      ),
    );
    if (confirmed != true) return;

    try {
      await ref.read(apiClientProvider).deleteDevice(_device['id'] as String);
      if (mounted) Navigator.of(context).pop();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể xóa thiết bị: ${friendlyError(e)}')));
      }
    }
  }

  Future<void> _updateFirmware() async {
    if (!ensureOnline(context, ref)) return;
    List<Map<String, dynamic>> builds;
    try {
      builds = await ref.read(apiClientProvider).listFirmwareBuilds();
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể tải danh sách firmware: ${friendlyError(e)}')));
      }
      return;
    }
    if (builds.isEmpty) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Chưa có bản firmware nào')));
      }
      return;
    }
    // adminGroup.MapGet("/firmware") returns newest first -- see Program.cs.
    final latest = builds.first;
    final latestVersion = latest['version'] as String;

    if (!mounted) return;
    if (latestVersion == _firmwareVersion) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Thiết bị đã là phiên bản mới nhất ($latestVersion)')),
      );
      return;
    }
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Cập nhật firmware'),
        content: Text(
          'Phiên bản hiện tại: ${_firmwareVersion ?? "chưa rõ"}\n'
          'Phiên bản mới: $latestVersion\n\n'
          'Thiết bị sẽ tải và cài firmware mới qua Wi-Fi, sau đó khởi động lại.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Hủy')),
          FilledButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Cập nhật')),
        ],
      ),
    );
    if (confirmed != true) return;

    try {
      await ref.read(apiClientProvider).triggerOta(_deviceId, latest['id'] as String);
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Đã gửi lệnh cập nhật firmware')));
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể cập nhật firmware: ${friendlyError(e)}')));
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final device = _device;
    final reading = device['latestReading'] as Map<String, dynamic>?;
    final soilMoisture = (reading?['soilMoisturePercent'] as num?)?.toDouble();
    final temperature = (reading?['temperatureCelsius'] as num?)?.toDouble();
    final humidity = (reading?['humidityPercent'] as num?)?.toDouble();
    final waterLevel = (reading?['waterLevelPercent'] as num?)?.toDouble();

    final soilStatus = _statusFor(soilMoisture, low: 30, high: 60);
    final tempStatus = _statusFor(temperature, low: 18, high: 30);
    final humidityStatus = _statusFor(humidity, low: 40, high: 80);
    final waterStatus = _statusFor(waterLevel, low: 20, high: 100);

    final overallGood = [soilStatus, tempStatus, humidityStatus, waterStatus].every((s) => s.label == 'Tốt');
    // Computed server-side (Program.cs's ComputeCareSuggestion) from the same
    // thresholds this screen already applies to soilStatus/tempStatus/etc. --
    // moved off the client so every client shares one source of truth.
    final suggestion = device['careSuggestion'] as String? ?? 'Đang cập nhật gợi ý chăm sóc...';
    final baseUrl = ref.read(apiClientProvider).baseUrl;

    return Scaffold(
      backgroundColor: DeviceDetailScreen.backgroundColor,
      appBar: AppBar(
        backgroundColor: DeviceDetailScreen.backgroundColor,
        foregroundColor: Colors.white,
        title: Row(
          children: [
            Flexible(
              child: Text(_name,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 20)),
            ),
            IconButton(icon: const Icon(Icons.edit, size: 20), tooltip: 'Đổi tên', onPressed: _renameDevice),
          ],
        ),
        actions: [
          if (_isAdmin)
            IconButton(icon: const Icon(Icons.system_update), tooltip: 'Cập nhật firmware', onPressed: _updateFirmware),
          IconButton(icon: const Icon(Icons.delete_outline), tooltip: 'Xóa thiết bị', onPressed: _deleteDevice),
        ],
      ),
      body: SafeArea(
        child: RefreshIndicator(
          onRefresh: _reload,
          color: DeviceDetailScreen.goodColor,
          backgroundColor: DeviceDetailScreen.cardColor,
          child: ListView(
          physics: const AlwaysScrollableScrollPhysics(),
          padding: const EdgeInsets.fromLTRB(20, 8, 20, 24),
          children: [
            Row(
              children: [
                const Text('Trạng thái: ', style: TextStyle(color: Colors.white70, fontSize: 15)),
                Text(overallGood ? 'Tốt' : 'Cần chú ý',
                    style: TextStyle(
                        color: overallGood ? DeviceDetailScreen.goodColor : DeviceDetailScreen.lowColor,
                        fontWeight: FontWeight.bold,
                        fontSize: 15)),
                const SizedBox(width: 12),
                Container(
                  width: 8,
                  height: 8,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: _isOnline ? DeviceDetailScreen.goodColor : Colors.white38,
                  ),
                ),
                const SizedBox(width: 6),
                Text(_isOnline ? 'Trực tuyến' : 'Ngoại tuyến',
                    style: const TextStyle(color: Colors.white70, fontSize: 13)),
              ],
            ),
            const SizedBox(height: 28),
            Center(
              child: GestureDetector(
                onTap: _uploadingAvatar ? null : _pickAndUploadAvatar,
                child: Container(
                  width: 160,
                  height: 160,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: const Color(0xFF163826),
                    border: Border.all(color: const Color(0xFF2E5A3E), width: 3),
                    image: _avatarUrl != null
                        ? DecorationImage(image: NetworkImage('$baseUrl$_avatarUrl'), fit: BoxFit.cover)
                        : null,
                  ),
                  child: _uploadingAvatar
                      ? const Center(child: CircularProgressIndicator(color: DeviceDetailScreen.goodColor))
                      : _avatarUrl == null
                          ? const Icon(Icons.local_florist, size: 76, color: DeviceDetailScreen.goodColor)
                          : Align(
                              alignment: Alignment.bottomRight,
                              child: Container(
                                margin: const EdgeInsets.all(8),
                                padding: const EdgeInsets.all(6),
                                decoration: const BoxDecoration(color: Colors.black54, shape: BoxShape.circle),
                                child: const Icon(Icons.camera_alt, size: 18, color: Colors.white),
                              ),
                            ),
                ),
              ),
            ),
            const SizedBox(height: 20),
            Center(
              child: Wrap(
                spacing: 8,
                runSpacing: 8,
                alignment: WrapAlignment.center,
                children: [
                  FilledButton.icon(
                    style: FilledButton.styleFrom(
                      backgroundColor: DeviceDetailScreen.highColor,
                      foregroundColor: DeviceDetailScreen.backgroundColor,
                    ),
                    onPressed: _wateringNow ? null : _triggerManualWatering,
                    icon: _wateringNow
                        ? const SizedBox(
                            width: 16,
                            height: 16,
                            child:
                                CircularProgressIndicator(strokeWidth: 2, color: DeviceDetailScreen.backgroundColor))
                        : const Icon(Icons.water_drop),
                    label: const Text('Tưới ngay'),
                  ),
                  OutlinedButton.icon(
                    onPressed: _pinging ? null : _pingDevice,
                    icon: _pinging
                        ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
                        : const Icon(Icons.campaign_outlined),
                    label: const Text('Tìm thiết bị'),
                  ),
                  OutlinedButton.icon(
                    onPressed: _togglingMuted ? null : _toggleMuted,
                    icon: _togglingMuted
                        ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2))
                        : Icon(_muted ? Icons.volume_off : Icons.volume_up),
                    label: Text(_muted ? 'Đang tắt tiếng' : 'Tắt tiếng'),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 12),
            SizedBox(
              width: double.infinity,
              child: FilledButton.icon(
                style: FilledButton.styleFrom(
                  backgroundColor: DeviceDetailScreen.goodColor,
                  foregroundColor: DeviceDetailScreen.backgroundColor,
                  padding: const EdgeInsets.symmetric(vertical: 14),
                ),
                onPressed: _openAssistantTalk,
                icon: const Icon(Icons.mic),
                label: const Text('Trò chuyện với Synora'),
              ),
            ),
            const SizedBox(height: 8),
            // A plain Column of Rows, not GridView.count(childAspectRatio:...)
            // -- a fixed aspect ratio forces a FIXED cell height derived only
            // from width, while _MetricTile's actual height depends on its
            // text content (which varies with the system font/text-scale
            // setting). Once real content needed more height than the fixed
            // ratio allocated, Flutter handed the tile impossible (min height
            // > max height) constraints and crashed outright -- Expanded here
            // only constrains width equally, so each tile sizes its own
            // height naturally regardless of text scale.
            Row(
              children: [
                Expanded(
                  child: _MetricTile(
                    icon: Icons.water_drop,
                    iconColor: DeviceDetailScreen.highColor,
                    label: 'Độ ẩm đất',
                    value: soilMoisture != null ? '${soilMoisture.toStringAsFixed(0)}%' : '—',
                    status: soilStatus,
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _MetricTile(
                    icon: Icons.thermostat,
                    iconColor: const Color(0xFFFFA726),
                    label: 'Nhiệt độ',
                    value: temperature != null ? '${temperature.toStringAsFixed(0)}°C' : '—',
                    status: tempStatus,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: _MetricTile(
                    icon: Icons.air,
                    iconColor: const Color(0xFF81D4FA),
                    label: 'Độ ẩm không khí',
                    value: humidity != null ? '${humidity.toStringAsFixed(0)}%' : '—',
                    status: humidityStatus,
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: _MetricTile(
                    icon: Icons.waves,
                    iconColor: const Color(0xFF4DD0E1),
                    label: 'Mực nước',
                    value: waterLevel != null ? '${waterLevel.toStringAsFixed(0)}%' : '—',
                    status: waterStatus,
                    isWarning: _lowWaterWarning != null && waterLevel != null && waterLevel <= _lowWaterWarning!,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 20),
            Container(
              padding: const EdgeInsets.all(16),
              decoration: BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(20)),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Row(
                    children: [
                      Icon(Icons.eco, color: DeviceDetailScreen.goodColor, size: 20),
                      SizedBox(width: 8),
                      Text('Gợi ý chăm sóc bởi AI', style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
                    ],
                  ),
                  const SizedBox(height: 10),
                  Text(suggestion, style: const TextStyle(color: Colors.white70, height: 1.4)),
                ],
              ),
            ),
            const SizedBox(height: 20),
            Container(
              padding: const EdgeInsets.all(16),
              decoration: BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(20)),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      const Icon(Icons.schedule, color: DeviceDetailScreen.highColor, size: 20),
                      const SizedBox(width: 8),
                      const Expanded(
                        child: Text('Lịch tưới tự động',
                            style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
                      ),
                      IconButton(
                        icon: const Icon(Icons.add_circle_outline, color: Colors.white70),
                        tooltip: 'Thêm lịch tưới',
                        onPressed: _addSchedule,
                      ),
                    ],
                  ),
                  if (_schedules.isEmpty)
                    const Padding(
                      padding: EdgeInsets.only(top: 4),
                      child: Text('Chưa có lịch tưới nào.', style: TextStyle(color: Colors.white70)),
                    )
                  else
                    ..._schedules.map((s) {
                      final hour = (s['hour'] as int).toString().padLeft(2, '0');
                      final minute = (s['minute'] as int).toString().padLeft(2, '0');
                      final enabled = s['enabled'] as bool;
                      return ListTile(
                        contentPadding: EdgeInsets.zero,
                        onTap: () => _editSchedule(s),
                        title: Text('$hour:$minute', style: const TextStyle(color: Colors.white)),
                        subtitle: Text('${s['durationSeconds']} giây',
                            style: const TextStyle(color: Colors.white70)),
                        trailing: Row(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            // shrinkWrap -- ListTile lays out trailing with a
                            // tight height derived from the tile's own
                            // (title+subtitle) content, and Switch's default
                            // 48dp minimum touch target can exceed that,
                            // handing the tile impossible constraints.
                            Switch(
                              value: enabled,
                              onChanged: (v) => _toggleSchedule(s, v),
                              materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                            ),
                            IconButton(
                              icon: const Icon(Icons.delete_outline, color: Colors.white70),
                              onPressed: () => _deleteSchedule(s['id'] as String),
                              constraints: const BoxConstraints(),
                              padding: const EdgeInsets.only(left: 8),
                              visualDensity: VisualDensity.compact,
                            ),
                          ],
                        ),
                      );
                    }),
                ],
              ),
            ),
            const SizedBox(height: 20),
            GestureDetector(
              onTap: _editLowWaterAlerts,
              child: Container(
                padding: const EdgeInsets.all(16),
                decoration:
                    BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(20)),
                child: Row(
                  children: [
                    const Icon(Icons.notifications_outlined, color: DeviceDetailScreen.lowColor, size: 20),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Text('Cảnh báo mực nước thấp',
                              style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
                          Text(
                            _lowWaterWarning == null && _lowWaterCritical == null
                                ? 'Đang tắt (chạm để cài đặt)'
                                : [
                                    if (_lowWaterWarning != null) 'Cảnh báo dưới $_lowWaterWarning%',
                                    if (_lowWaterCritical != null)
                                      'Khẩn cấp dưới $_lowWaterCritical%${_intervalLabel()}',
                                  ].join(' · '),
                            style: const TextStyle(color: Colors.white70, fontSize: 13),
                          ),
                        ],
                      ),
                    ),
                    const Icon(Icons.chevron_right, color: Colors.white38),
                  ],
                ),
              ),
            ),
          ],
          ),
        ),
      ),
    );
  }

  static _MetricStatus _statusFor(double? value, {required double low, required double high}) {
    if (value == null) return const _MetricStatus('—', Colors.white38);
    if (value < low) return const _MetricStatus('Thấp', DeviceDetailScreen.lowColor);
    if (value > high) return const _MetricStatus('Cao', DeviceDetailScreen.highColor);
    return const _MetricStatus('Tốt', DeviceDetailScreen.goodColor);
  }
}

class _MetricStatus {
  const _MetricStatus(this.label, this.color);
  final String label;
  final Color color;
}

// Clamps digit-only input to [minValue, 100] as the user types, rather than
// only validating on save.
class _PercentInputFormatter extends TextInputFormatter {
  _PercentInputFormatter({this.minValue = 0});
  final int minValue;

  @override
  TextEditingValue formatEditUpdate(TextEditingValue oldValue, TextEditingValue newValue) {
    final digitsOnly = newValue.text.replaceAll(RegExp(r'[^0-9]'), '');
    if (digitsOnly.isEmpty) {
      return newValue.copyWith(text: '');
    }
    final clamped = int.parse(digitsOnly).clamp(minValue, 100).toString();
    return TextEditingValue(text: clamped, selection: TextSelection.collapsed(offset: clamped.length));
  }
}

class _MetricTile extends StatelessWidget {
  const _MetricTile({
    required this.icon,
    required this.iconColor,
    required this.label,
    required this.value,
    required this.status,
    this.isWarning = false,
  });

  final IconData icon;
  final Color iconColor;
  final String label;
  final String value;
  final _MetricStatus status;
  // In-app-only low-water warning (independent of the critical/push
  // threshold) -- highlights this tile yellow with a warning icon.
  final bool isWarning;

  static const _warningColor = Color(0xFFFFC107);

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: isWarning ? _warningColor.withValues(alpha: 0.18) : DeviceDetailScreen.cardColor,
        borderRadius: BorderRadius.circular(20),
        border: isWarning ? Border.all(color: _warningColor, width: 1.5) : null,
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Row(
            children: [
              Icon(icon, color: iconColor, size: 22),
              if (isWarning) ...[
                const SizedBox(width: 6),
                const Icon(Icons.warning_amber_rounded, color: _warningColor, size: 18),
              ],
            ],
          ),
          const SizedBox(height: 10),
          Text(label, style: const TextStyle(color: Colors.white70, fontSize: 13)),
          const SizedBox(height: 4),
          Text(value, style: const TextStyle(color: Colors.white, fontSize: 22, fontWeight: FontWeight.bold)),
          const SizedBox(height: 4),
          Text(status.label, style: TextStyle(color: status.color, fontSize: 12, fontWeight: FontWeight.w600)),
        ],
      ),
    );
  }
}
