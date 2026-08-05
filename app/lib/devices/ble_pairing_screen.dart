import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

import '../api_client.dart';
import '../main.dart';
import 'device_detail_screen.dart';

class _WifiNetwork {
  _WifiNetwork({required this.ssid, required this.rssi});
  final String ssid;
  final int rssi;
}

/// Must match the UUIDs in firmware/main/boards/common/ble_provisioning.cc exactly.
class SynoraBleUuids {
  static final service = Guid('d2270ba0-a51c-4d67-8253-08367721e4e7');
  static final wifiSsid = Guid('f0cc2af8-8477-443a-a981-aa1ea57655c6');
  static final wifiPassword = Guid('518c94dd-3b10-4ccc-96a4-30bb859d2620');
  static final wifiScan = Guid('b5233f6f-0b5d-4ac0-983c-c5c7978f4476');
  static final serverUrl = Guid('1413dc99-e1fd-4991-9c88-6d8e535dbf1a');
  static final deviceInfo = Guid('ca17f48a-2be1-456b-934e-96440927b009');
  static final settings = Guid('eeb5de90-1c07-402a-933c-bc0c3149a3d3');
  static final activation = Guid('b2af9052-c495-4064-a949-a4ff16f17572');
  static final control = Guid('0d128bcf-fd9d-4288-97fb-b11b430e45aa');
  static final status = Guid('483d3f05-a014-4990-a511-fc9863763cca');
}

enum _Step { permissions, scanning, connecting, alreadyPaired, wifiForm, joining, activating, done, error }

// Shown only on a server-confirmed 409 (the real, authoritative check) --
// unlike the BLE deviceInfo isPaired pre-check, which is just the device's
// own possibly-stale local cache and no longer blocks pairing outright (see
// _Step.alreadyPaired's "continue anyway" option).
const _kAlreadyPairedMessage = 'Thiết bị này đã được ghép nối với một tài khoản khác.\n\n'
    'Nếu đây là thiết bị của bạn, hãy giữ nút trên thiết bị trong 10 giây để đặt lại và hủy liên kết trước khi ghép nối lại.';

// Onboarding wizard from DESIGN.md section 5: scan for "Synora-XXXX" -> connect ->
// pick Wi-Fi -> write credentials -> commit -> watch status -> read activation code
// -> auto-claim via the REST API -> done.
//
// The scan lists every nearby BLE device (not just ones already named "Synora"),
// since without real firmware to pair against there'd otherwise be nothing to see —
// this also lets you confirm scanning/permissions work end-to-end on real hardware.
class BlePairingScreen extends ConsumerStatefulWidget {
  const BlePairingScreen({this.onPaired, super.key});

  // Called right when pairDevice() succeeds -- decoupled from navigation
  // timing (the auto-pop below, or however the user later backs out) so the
  // device list reloads immediately instead of depending on Navigator.push()'s
  // returned Future actually resolving.
  final VoidCallback? onPaired;

  @override
  ConsumerState<BlePairingScreen> createState() => _BlePairingScreenState();
}

class _BlePairingScreenState extends ConsumerState<BlePairingScreen> {
  _Step _step = _Step.permissions;
  BluetoothDevice? _device;
  final _ssidController = TextEditingController();
  final _passwordController = TextEditingController();
  final _passwordFocusNode = FocusNode();
  String? _error;
  final Map<DeviceIdentifier, ScanResult> _discovered = {};
  StreamSubscription<List<ScanResult>>? _scanSub;
  bool _scanFinished = false;
  List<_WifiNetwork> _wifiNetworks = [];
  bool _scanningWifi = false;
  bool _obscureWifiPassword = true;
  // False only for the very first scan right after connecting -- shows a
  // dedicated full-screen loading state instead of the form with an
  // empty-looking dropdown, which was easy to mistake for "no networks
  // found" rather than "still scanning."
  bool _initialWifiScanDone = false;

  // Bumped by _cancelAndRestart() to invalidate whatever _submitWifi()/
  // _pairWithCode()/_reactivateAndRetryPair() call is currently in flight --
  // those only apply a setState() if their own captured generation still
  // matches this, so a stale awaited BLE/HTTP response from a cancelled
  // attempt can't clobber the UI after the user has already moved on. Cheap
  // cancellation-by-ignoring rather than a true cancellation token: the
  // orphaned call keeps running in the background (harmlessly) until its own
  // timeout/completion, silently, instead of being forcibly torn down.
  int _submitGeneration = 0;

  @override
  void initState() {
    super.initState();
    // Pairing involves several waits that can each run tens of seconds
    // (Wi-Fi scan chunks, ConnectAndActivate's connect timeout) -- the screen
    // locking mid-flow interrupts BLE I/O on some devices and just looks like
    // the pairing hung. Released in dispose() regardless of how this screen
    // is exited (success, back button, or an error).
    WakelockPlus.enable();
    _requestPermissionsThenScan();
  }

  @override
  void dispose() {
    WakelockPlus.disable();
    _passwordFocusNode.dispose();
    _scanSub?.cancel();
    FlutterBluePlus.stopScan();
    // Backing out mid-flow (e.g. after an error) previously left the BLE
    // connection open in the background -- disconnect explicitly so it
    // doesn't linger and tie up the device's single BLE connection slot.
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

    // BLUETOOTH_SCAN is declared with usesPermissionFlags="neverForLocation" in
    // AndroidManifest.xml, so Android 12+ doesn't need location for BLE scanning
    // at all. ACCESS_FINE_LOCATION is manifest-scoped to maxSdkVersion 30, so on
    // newer OS versions it isn't even declared — permission_handler reports it
    // denied no matter what, which isn't a real problem. Only Bluetooth
    // scan/connect are mandatory here.
    final bluetoothGranted =
        statuses[Permission.bluetoothScan]!.isGranted && statuses[Permission.bluetoothConnect]!.isGranted;

    if (!bluetoothGranted && statuses.values.any((s) => s.isPermanentlyDenied)) {
      if (!mounted) return;
      setState(() {
        _step = _Step.error;
        _error = 'Quyền Bluetooth đã bị từ chối. Vui lòng bật trong Cài đặt và thử lại.';
      });
      return;
    }
    if (!bluetoothGranted) {
      if (!mounted) return;
      setState(() {
        _step = _Step.error;
        _error = 'Cần có quyền Bluetooth để ghép nối thiết bị.';
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
            if (r.device.platformName.isNotEmpty) {
              _discovered[r.device.remoteId] = r;
            }
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
    await _connect();
  }

  // "Thử lại" from the error step -- reconnects to the SAME device instead
  // of restarting the whole flow with a fresh BLE scan. The device itself
  // already re-advertises on its own the moment anything (a failed Wi-Fi
  // connect attempt, an activation timeout, ...) drops the BLE link, so
  // there's a real device to reconnect to; rescanning just made the user
  // wait through another 15s discovery + re-pick the same entry from the
  // list for no reason. Falls back to a full rescan only if _device somehow
  // isn't set yet (shouldn't happen once past _Step.scanning, but this
  // button is reachable from the permission-denied error too).
  Future<void> _retry() async {
    if (_device == null) {
      await _requestPermissionsThenScan();
      return;
    }
    setState(() => _error = null);
    try {
      await _connect();
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _step = _Step.error;
        _error = e.toString();
      });
    }
  }

  Future<void> _connect() async {
    if (!mounted) return;
    setState(() => _step = _Step.connecting);
    await _device!.connect();
    final services = await _device!.discoverServices();

    // Best-effort/eventually-consistent (cached on the device from its last
    // telemetry heartbeat, not a live server query) -- but catches the
    // common case of a device that's already paired yet still briefly
    // BLE-connectable (the gap before its own next heartbeat tears BLE
    // down), sparing the user filling in the whole Wi-Fi form only to hit a
    // 409 at the very end.
    try {
      final service = services.firstWhere((s) => s.uuid == SynoraBleUuids.service);
      final chars = {for (final c in service.characteristics) c.uuid: c};
      final infoBytes = await chars[SynoraBleUuids.deviceInfo]!.read();
      final info = jsonDecode(String.fromCharCodes(infoBytes)) as Map<String, dynamic>;
      if (info['isPaired'] == true) {
        if (!mounted) return;
        setState(() => _step = _Step.alreadyPaired);
        return;
      }
    } catch (_) {
      // Non-fatal -- fall through to the normal Wi-Fi form if this can't be
      // determined for any reason.
    }

    _proceedToWifiForm();
  }

  void _proceedToWifiForm() {
    if (!mounted) return;
    setState(() => _step = _Step.wifiForm);
    // Actively rescan rather than just reading whatever's cached -- the
    // device's own background scan backs off exponentially (up to 5 min)
    // once nothing matches a known SSID, which is the common case here, so a
    // passive read could serve minutes-stale (or still-empty) data. This was
    // the real cause of the Wi-Fi list being "flaky, not every time it has
    // data" on first open, not just on the manual "Quét lại" tap.
    unawaited(_rescanWifiNetworks().whenComplete(() {
      if (mounted) setState(() => _initialWifiScanDone = true);
    }));
  }

  // Asks the device to run a fresh scan (BleProvisioning::HandleRescan) and
  // waits for it to actually finish -- the spinner keeps spinning until the
  // device notifies wifi_scan with the new results, instead of guessing a
  // fixed delay or serving a passively-cached (possibly stale/empty) list.
  Future<void> _rescanWifiNetworks() async {
    if (!mounted) return;
    setState(() => _scanningWifi = true);
    try {
      final services = await _device!.discoverServices();
      final service = services.firstWhere((s) => s.uuid == SynoraBleUuids.service);
      final chars = {for (final c in service.characteristics) c.uuid: c};
      final scanChar = chars[SynoraBleUuids.wifiScan]!;
      await scanChar.setNotifyValue(true);

      var networks = <_WifiNetwork>[];
      // Up to 4 attempts, with a pause in between -- a scan while BLE is
      // actively connected can genuinely come back with 0 APs (observed
      // live, repeatedly: ESP32-S3 has a single shared 2.4GHz radio, and
      // Wi-Fi/BLE coexistence can starve the Wi-Fi scan of airtime while an
      // active BLE connection's timing events keep interrupting it -- a
      // scan run immediately after BLE *disconnected* found real networks
      // in under half a second, confirming the radio itself is fine).
      // Back-to-back retries with no delay also triggered a distinct
      // Android-side GATT_INVALID_OFFSET on the following read (the BLE
      // stack seemingly needs a moment to settle between reads of a
      // characteristic whose value length just changed), so this pauses
      // briefly between attempts -- both giving the coexistence scheduler
      // room to let a scan through cleanly, and letting Android's GATT
      // state settle before the next read.
      //
      // A per-attempt try/catch, not just `networks.isEmpty` -- a busy scan's
      // JSON response was observed live getting truncated at exactly 512
      // bytes (a single-GATT-read size limit), throwing a FormatException
      // that used to escape this loop entirely and skip the retry outright.
      // The firmware now paginates instead, but this stays as a safety net
      // against any other transient failure (like GATT_INVALID_OFFSET above).
      for (var attempt = 0; attempt < 4 && networks.isEmpty; attempt++) {
        if (attempt > 0) {
          await Future.delayed(const Duration(milliseconds: 700));
        }
        try {
          networks = await _requestWifiScan(chars, scanChar);
        } catch (_) {
          // Retried below.
        }
      }
      // Only replace the list on a genuinely non-empty result -- a manual
      // "Quét lại" that ends up failing/empty (e.g. a transient scan hiccup)
      // used to wipe out a previously-successful list, which looked exactly
      // like "rescan makes the list empty" even though the earlier data was
      // still perfectly good. Stale-but-real beats a blank list.
      if (mounted && networks.isNotEmpty) setState(() => _wifiNetworks = networks);
    } catch (_) {
      // Non-fatal — the network field still accepts manual entry.
    } finally {
      if (mounted) setState(() => _scanningWifi = false);
    }
  }

  // A full rescan: "rescan" (fresh scan, resets the device's pagination
  // cursor to the top) for the first chunk, then "more_aps" (no new scan,
  // just serves the next chunk) repeatedly while the device reports more
  // remain -- a single BLE GATT attribute value is hard-capped at 512 bytes
  // (Bluetooth Core Spec Vol 3, Part F, 3.2.9) regardless of MTU or read
  // chunking, and a busy scan's full list can exceed that in one shot, so
  // the device paginates and this fetches every chunk instead of only the
  // first, so no visible network is ever silently dropped.
  Future<List<_WifiNetwork>> _requestWifiScan(
    Map<Guid, BluetoothCharacteristic> chars,
    BluetoothCharacteristic scanChar,
  ) async {
    final networks = <_WifiNetwork>[];
    var command = 'rescan';
    var hasMore = true;
    while (hasMore) {
      final chunk = await _requestWifiScanChunk(chars, scanChar, command);
      networks.addAll(chunk.networks);
      hasMore = chunk.hasMore;
      command = 'more_aps';
    }
    return networks
        .where((n) => n.ssid.isNotEmpty)
        .toList()
      ..sort((a, b) => b.rssi.compareTo(a.rssi));
  }

  // One chunk round trip: write [command], wait for the device's fresh
  // notify, then read the characteristic for that chunk's (size-bounded)
  // payload.
  Future<({List<_WifiNetwork> networks, bool hasMore})> _requestWifiScanChunk(
    Map<Guid, BluetoothCharacteristic> chars,
    BluetoothCharacteristic scanChar,
    String command,
  ) async {
    // A real Wi-Fi scan on the device can occasionally take longer than
    // expected (observed ~16.5s once, live). Worst case on the firmware side
    // is up to ~40s: a 30s scan timeout, a fixed 2s settle delay, then an 8s
    // retry scan if the first one came back empty (BleProvisioning::
    // HandleRescan) -- this wait must stay comfortably above that, or a slow
    // scan gets silently discarded partway through and just looks hung.
    // Using `.first.timeout(...)` here was a separate real bug: once that
    // Future's timeout fires, Dart discards
    // whatever the underlying stream produces afterward, even a moment
    // later, so a scan that finishes just past the deadline was silently
    // thrown away and the list stayed empty. Listening persistently for a
    // signal (instead of a single racing Future) avoids that.
    //
    // The notify's own PAYLOAD is deliberately ignored -- a BLE notification
    // is a single unfragmented packet capped at (MTU-3) bytes (confirmed
    // live at only 256 MTU = 253 usable bytes), well under one chunk's
    // worst case, so it's used only as a "this chunk is ready" signal. A
    // characteristic *read* (below) is used to actually fetch the chunk.
    //
    // lastValueStream replays the last known value immediately to new
    // subscribers (by design, for UI binding) -- using it here was a real
    // bug: it fired the completer instantly on stale/cached data (e.g. from
    // the very first passive read on connect) instead of waiting for this
    // request's genuinely new notify, so the follow-up read() raced ahead
    // and fetched old data almost every time. onValueReceived is the raw
    // per-event stream with no replay, so it only fires for an
    // actually-new notification.
    final completer = Completer<void>();
    final sub = scanChar.onValueReceived.listen((_) {
      if (!completer.isCompleted) completer.complete();
    });
    try {
      await chars[SynoraBleUuids.control]!.write(command.codeUnits, withoutResponse: false);
      await completer.future.timeout(const Duration(seconds: 45), onTimeout: () {});

      final bytes = await scanChar.read();
      final rawString = String.fromCharCodes(bytes);
      final decoded = jsonDecode(rawString) as Map<String, dynamic>;
      final items = decoded['items'] as List;
      // Each entry is a compact [ssid, rssi] array, not an
      // {"ssid":...,"rssi":...} object -- saves per-entry key-name bytes,
      // which matters since each chunk has to fit under BLE's hard
      // 512-byte attribute-value ceiling (see ScanWifiNetworksJson).
      final networks = items.map((e) => _WifiNetwork(ssid: e[0] as String, rssi: e[1] as int)).toList();
      return (networks: networks, hasMore: decoded['more'] as bool);
    } finally {
      await sub.cancel();
    }
  }

  // True if this attempt has been superseded (cancelled, or another attempt
  // started) since it began -- see _submitGeneration's own doc comment.
  bool _isStale(int myGeneration) => !mounted || myGeneration != _submitGeneration;

  // "Hủy" on the joining/activating screens -- those steps otherwise have no
  // way out except waiting out their own 120s timeouts. Bumping
  // _submitGeneration makes every setState() in the in-flight
  // _submitWifi()/_pairWithCode()/_reactivateAndRetryPair() call a no-op from
  // here on, so jumping straight back to the Wi-Fi form is safe even though
  // that old call is still technically running in the background.
  void _cancelAndRestart() {
    setState(() {
      _submitGeneration++;
      _step = _Step.wifiForm;
      _error = null;
    });
  }

  Future<void> _submitWifi() async {
    if (!mounted) return;
    final myGeneration = ++_submitGeneration;
    setState(() => _step = _Step.joining);
    try {
      final services = await _device!.discoverServices();
      final service = services.firstWhere((s) => s.uuid == SynoraBleUuids.service);
      final chars = {for (final c in service.characteristics) c.uuid: c};

      final statusChar = chars[SynoraBleUuids.status]!;
      await statusChar.setNotifyValue(true);
      // onValueReceived, not lastValueStream -- lastValueStream replays
      // whatever value is already cached (e.g. an "activated" status left
      // over from a prior pairing session with this same device) the instant
      // this subscribes, resolving activatedOrError/onlineOrError below
      // before this attempt's own Wi-Fi connect+activate has even run. That
      // let the follow-up activationChar.read() race ahead and pick up a
      // stale/already-superseded code, which 404s against /devices/pair once
      // the real (fresh) code belongs to a different, newly-created row.
      final statusUpdates = statusChar.onValueReceived.map((bytes) => String.fromCharCodes(bytes));

      // Without this, the device's server_url stays empty and
      // DeviceActivation::TryActivate fails immediately on commit.
      final serverUrl = ref.read(apiClientProvider).baseUrl;
      await chars[SynoraBleUuids.serverUrl]!.write(serverUrl.codeUnits, withoutResponse: false);
      await chars[SynoraBleUuids.wifiSsid]!.write(_ssidController.text.codeUnits, withoutResponse: false);
      await chars[SynoraBleUuids.wifiPassword]!.write(_passwordController.text.codeUnits, withoutResponse: false);
      await chars[SynoraBleUuids.control]!.write('commit'.codeUnits, withoutResponse: false);

      // Two independently-budgeted stages instead of one flat deadline: the
      // device's Wi-Fi connect attempt alone can take up to 30s (see
      // BleProvisioning::ConnectAndActivate's timeout), the BLE reconnect
      // dance in between (central disconnect -> re-advertise -> this app
      // reconnects -> rediscovers services -> resubscribes) adds more on top
      // of that, and the HTTPS activation call afterward needs its own DNS +
      // TCP + TLS + request/response round trip. Each stage gets a generous
      // 120s -- confirmed live, repeatedly, that tighter budgets (45s flat,
      // then 35s/30s per stage, then 50s/30s) kept giving up while the
      // device was still genuinely working through a slow but real network
      // (this account's Wi-Fi/backend has been intermittently very slow this
      // session), showing a false timeout error instead of just waiting.
      //
      // One subscription with two completers (rather than two sequential
      // firstWhere() awaits) so a fast device that reaches "activated" before
      // this code starts awaiting the second stage still gets caught — no gap
      // between listeners.
      final onlineOrError = Completer<String>();
      final activatedOrError = Completer<String>();
      void handleStatus(String s) {
        // 'activated' also resolves onlineOrError, not just 'online'/'error'
        // -- the device flips to 'online' right as it reconnects BLE after
        // its own deliberate Wi-Fi-attempt disconnect (see
        // ConnectAndActivate()'s comment), and that notification has a real
        // chance of firing before this app's central has actually
        // reconnected, with nobody subscribed to receive it. Reaching
        // 'activated' is only possible after the device was online, so
        // treat it as proof online already happened even if that specific
        // notification got lost -- confirmed live: without this, a device
        // that WiFi-connected in ~130ms and activated successfully still
        // left the app stuck on onlineOrError's own 120s timeout, showing a
        // misleading "Wi-Fi connect timeout" error.
        if (!onlineOrError.isCompleted && (s == 'online' || s == 'activated' || s == 'error')) {
          onlineOrError.complete(s == 'activated' ? 'online' : s);
        }
        if (!activatedOrError.isCompleted && (s == 'activated' || s == 'error')) {
          activatedOrError.complete(s);
        }
      }

      final sub = statusUpdates.listen(handleStatus);

      // The device deliberately drops this BLE connection right as it starts
      // its own Wi-Fi connect attempt, to free the radio from coexistence
      // contention (see BleProvisioning::ConnectAndActivate()'s own comment)
      // -- it re-advertises immediately once that disconnect completes, but
      // this app-side connection doesn't reconnect on its own. Without this,
      // the notify subscription above just goes silent forever and the whole
      // flow times out even though the device is working correctly.
      final reconnectSub = _device!.connectionState.listen((state) async {
        if (state != BluetoothConnectionState.disconnected) return;
        if (onlineOrError.isCompleted && activatedOrError.isCompleted) return;
        try {
          await _device!.connect();
          final freshServices = await _device!.discoverServices();
          final freshService = freshServices.firstWhere((s) => s.uuid == SynoraBleUuids.service);
          final freshStatusChar =
              freshService.characteristics.firstWhere((c) => c.uuid == SynoraBleUuids.status);
          await freshStatusChar.setNotifyValue(true);
          freshStatusChar.onValueReceived.map((bytes) => String.fromCharCodes(bytes)).listen(handleStatus);
        } catch (_) {
          // Non-fatal -- the outer timeouts below are the safety net if
          // reconnecting doesn't pan out.
        }
      });
      try {
        final wifiStatus = await onlineOrError.future
            .timeout(const Duration(seconds: 120), onTimeout: () => 'timeout');
        if (_isStale(myGeneration)) return;

        if (wifiStatus != 'online') {
          setState(() {
            _step = _Step.error;
            _error = wifiStatus == 'timeout'
                ? 'Hết thời gian chờ thiết bị kết nối Wi-Fi.'
                : 'Thiết bị báo lỗi khi kết nối Wi-Fi.';
          });
          return;
        }

        setState(() => _step = _Step.activating);

        final finalStatus = await activatedOrError.future
            .timeout(const Duration(seconds: 120), onTimeout: () => 'timeout');
        if (_isStale(myGeneration)) return;

        if (finalStatus != 'activated') {
          setState(() {
            _step = _Step.error;
            _error = finalStatus == 'timeout'
                ? 'Hết thời gian chờ máy chủ xác nhận thiết bị.'
                : 'Thiết bị báo lỗi khi kích hoạt.';
          });
          return;
        }
      } finally {
        await sub.cancel();
        await reconnectSub.cancel();
      }

      if (_isStale(myGeneration)) return;

      // Re-discovered fresh (not reusing the `chars` map from before the
      // wait above) -- the device may have disconnected/reconnected during
      // that wait (see reconnectSub's own comment), and characteristic
      // references from before a reconnect aren't guaranteed valid on the
      // new connection.
      final finalServices = await _device!.discoverServices();
      final finalService = finalServices.firstWhere((s) => s.uuid == SynoraBleUuids.service);
      final finalChars = {for (final c in finalService.characteristics) c.uuid: c};

      final activationChar = finalChars[SynoraBleUuids.activation]!;
      final code = String.fromCharCodes(await activationChar.read());
      debugPrint('SYNORA_DEBUG: activation code read from BLE: "$code"');
      if (_isStale(myGeneration)) return;
      if (code.isEmpty) {
        setState(() {
          _step = _Step.error;
          _error = 'Thiết bị chưa gửi mã kích hoạt.';
        });
        return;
      }

      // Best-effort -- the server already captures the MAC at activation
      // time, so this is just a defensive backfill, not required for pairing
      // to succeed.
      String? macAddress;
      try {
        final infoBytes = await finalChars[SynoraBleUuids.deviceInfo]!.read();
        final info = jsonDecode(String.fromCharCodes(infoBytes)) as Map<String, dynamic>;
        final rawId = info['deviceId'] as String?; // 12 hex chars, no separators
        if (rawId != null && rawId.length == 12) {
          macAddress = RegExp('.{2}').allMatches(rawId).map((m) => m.group(0)).join(':').toUpperCase();
        }
      } catch (_) {
        // Non-fatal.
      }

      await _pairWithCode(code, macAddress, myGeneration);
    } catch (e) {
      if (_isStale(myGeneration)) return;
      setState(() {
        _step = _Step.error;
        _error = e is ApiException && e.statusCode == 409 ? _kAlreadyPairedMessage : e.toString();
      });
    }
  }

  Future<void> _pairWithCode(String code, String? macAddress, int myGeneration) async {
    debugPrint('SYNORA_DEBUG: pairing with code="$code" mac=$macAddress');
    try {
      await ref.read(apiClientProvider).pairDevice(code, macAddress: macAddress);
      // Called directly here rather than relying on Navigator.push()'s
      // returned Future resolving (which depends on exactly how/when this
      // route gets popped) -- this way the list refreshes the moment pairing
      // actually succeeds, regardless of navigation timing afterward.
      widget.onPaired?.call();
      if (_isStale(myGeneration)) return;
      setState(() => _step = _Step.done);
      // Auto-return to the device list. Give the user a moment to read the
      // success message first.
      Future.delayed(const Duration(seconds: 2), () {
        if (mounted) Navigator.of(context).pop();
      });
    } on ApiException catch (e) {
      debugPrint('SYNORA_DEBUG: pairDevice failed status=${e.statusCode} body=${e.toString()}');
      if (e.statusCode == 404) {
        // The device's cached activation code refers to a row that no longer
        // exists server-side (e.g. deleted via the web admin console) --
        // ask the device to mint a fresh one on the spot and retry, instead
        // of a dead-end error that used to require a full factory reset.
        await _reactivateAndRetryPair(macAddress, myGeneration);
        return;
      }
      rethrow;
    }
  }

  Future<void> _reactivateAndRetryPair(String? macAddress, int myGeneration) async {
    try {
      final services = await _device!.discoverServices();
      final service = services.firstWhere((s) => s.uuid == SynoraBleUuids.service);
      final chars = {for (final c in service.characteristics) c.uuid: c};

      final activationChar = chars[SynoraBleUuids.activation]!;
      await activationChar.setNotifyValue(true);
      final nextCode = activationChar.lastValueStream.first;

      await chars[SynoraBleUuids.control]!.write('reactivate'.codeUnits, withoutResponse: false);

      final bytes = await nextCode.timeout(const Duration(seconds: 15));
      final freshCode = String.fromCharCodes(bytes);
      if (_isStale(myGeneration)) return;
      if (freshCode.isEmpty) {
        setState(() {
          _step = _Step.error;
          _error = 'Thiết bị không thể lấy mã kích hoạt mới.';
        });
        return;
      }

      // Not recursing through _pairWithCode -- a second 404 in a row means
      // something is genuinely wrong server-side, not just a one-off stale
      // code, so surface it as an error instead of looping forever.
      await ref.read(apiClientProvider).pairDevice(freshCode, macAddress: macAddress);
      widget.onPaired?.call();
      if (_isStale(myGeneration)) return;
      setState(() => _step = _Step.done);
      Future.delayed(const Duration(seconds: 2), () {
        if (mounted) Navigator.of(context).pop();
      });
    } catch (e) {
      if (_isStale(myGeneration)) return;
      setState(() {
        _step = _Step.error;
        _error = e is ApiException && e.statusCode == 409 ? _kAlreadyPairedMessage : e.toString();
      });
    }
  }

  InputDecoration _fieldDecoration(String label) {
    return InputDecoration(
      labelText: label,
      labelStyle: const TextStyle(color: Colors.white70),
      filled: true,
      fillColor: DeviceDetailScreen.cardColor,
      border: OutlineInputBorder(borderRadius: BorderRadius.circular(12), borderSide: BorderSide.none),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: DeviceDetailScreen.backgroundColor,
      appBar: AppBar(
        backgroundColor: DeviceDetailScreen.backgroundColor,
        foregroundColor: Colors.white,
        title: const Text('Ghép nối thiết bị'),
      ),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: DefaultTextStyle.merge(
          style: const TextStyle(color: Colors.white),
          child: switch (_step) {
            _Step.permissions => const Center(child: Text('Đang yêu cầu quyền…')),
            _Step.scanning => _buildScanningView(),
            _Step.connecting => const Center(child: Text('Đang kết nối…')),
            _Step.alreadyPaired => Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Icon(Icons.info_outline, color: DeviceDetailScreen.highColor, size: 48),
                    const SizedBox(height: 16),
                    const Text('Thiết bị báo đã được ghép nối với một tài khoản khác.',
                        textAlign: TextAlign.center),
                    const SizedBox(height: 8),
                    const Text(
                      // This is the device's own locally-cached status, only
                      // confirmed the next time it's back online and checks
                      // in with the server -- if it's been offline (e.g. no
                      // Wi-Fi yet, which is exactly why you're pairing it),
                      // that cache can be stale. The real check happens when
                      // you submit pairing below, so it's safe to try anyway.
                      'Thông tin này lấy từ chính thiết bị và có thể đã cũ nếu thiết bị offline lâu -- việc ghép nối thực tế vẫn sẽ được máy chủ xác nhận lại, nên bạn vẫn có thể thử tiếp tục.',
                      textAlign: TextAlign.center,
                      style: TextStyle(color: Colors.white70, fontSize: 13),
                    ),
                    const SizedBox(height: 20),
                    FilledButton(
                      style: FilledButton.styleFrom(
                        backgroundColor: DeviceDetailScreen.goodColor,
                        foregroundColor: DeviceDetailScreen.backgroundColor,
                      ),
                      onPressed: _proceedToWifiForm,
                      child: const Text('Vẫn tiếp tục ghép nối'),
                    ),
                    const SizedBox(height: 8),
                    TextButton(
                      onPressed: () => Navigator.of(context).pop(),
                      child: const Text('Quay lại', style: TextStyle(color: Colors.white70)),
                    ),
                  ],
                ),
              ),
            _Step.wifiForm => !_initialWifiScanDone
                ? const Center(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        CircularProgressIndicator(color: DeviceDetailScreen.goodColor),
                        SizedBox(height: 16),
                        Text('Đang quét mạng Wi-Fi…'),
                      ],
                    ),
                  )
                : Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Row(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Expanded(
                            child: TextField(
                              controller: _ssidController,
                              style: const TextStyle(color: Colors.white),
                              decoration: _fieldDecoration('Mạng Wi-Fi'),
                            ),
                          ),
                          IconButton(
                            tooltip: 'Quét lại',
                            onPressed: _scanningWifi ? null : _rescanWifiNetworks,
                            icon: _scanningWifi
                                ? const SizedBox(
                                    width: 20,
                                    height: 20,
                                    child: CircularProgressIndicator(
                                        strokeWidth: 2, color: DeviceDetailScreen.goodColor))
                                : const Icon(Icons.refresh_rounded, color: Colors.white),
                          ),
                        ],
                      ),
                      const SizedBox(height: 12),
                      TextField(
                        controller: _passwordController,
                        focusNode: _passwordFocusNode,
                        style: const TextStyle(color: Colors.white),
                        decoration: _fieldDecoration('Mật khẩu Wi-Fi').copyWith(
                          suffixIcon: IconButton(
                            icon: Icon(_obscureWifiPassword ? Icons.visibility_off : Icons.visibility,
                                color: Colors.white70),
                            onPressed: () => setState(() => _obscureWifiPassword = !_obscureWifiPassword),
                          ),
                        ),
                        obscureText: _obscureWifiPassword,
                      ),
                      const SizedBox(height: 16),
                      FilledButton(
                        style: FilledButton.styleFrom(
                          backgroundColor: DeviceDetailScreen.goodColor,
                          foregroundColor: DeviceDetailScreen.backgroundColor,
                        ),
                        onPressed: _submitWifi,
                        child: const Text('Kết nối thiết bị với Wi-Fi'),
                      ),
                      const SizedBox(height: 16),
                      Text('Mạng lân cận (${_wifiNetworks.length})',
                          style: const TextStyle(color: Colors.white70, fontSize: 13)),
                      const SizedBox(height: 8),
                      Expanded(
                        child: _wifiNetworks.isEmpty
                            ? const Center(
                                child: Text('Không tìm thấy mạng nào.', style: TextStyle(color: Colors.white70)),
                              )
                            : ListView.builder(
                                itemCount: _wifiNetworks.length,
                                itemBuilder: (context, index) {
                                  final network = _wifiNetworks[index];
                                  final selected = _ssidController.text == network.ssid;
                                  return Container(
                                    margin: const EdgeInsets.only(bottom: 6),
                                    decoration: BoxDecoration(
                                      color: selected
                                          ? DeviceDetailScreen.goodColor.withValues(alpha: 0.18)
                                          : DeviceDetailScreen.cardColor,
                                      borderRadius: BorderRadius.circular(12),
                                      border: selected
                                          ? Border.all(color: DeviceDetailScreen.goodColor, width: 1.5)
                                          : null,
                                    ),
                                    child: ListTile(
                                      dense: true,
                                      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
                                      leading: const Icon(Icons.wifi, color: Colors.white70),
                                      title: Text(network.ssid, style: const TextStyle(color: Colors.white)),
                                      trailing: Text('${network.rssi} dBm',
                                          style: const TextStyle(color: Colors.white70, fontSize: 12)),
                                      onTap: () {
                                        setState(() => _ssidController.text = network.ssid);
                                        _passwordFocusNode.requestFocus();
                                      },
                                    ),
                                  );
                                },
                              ),
                      ),
                    ],
                  ),
            _Step.joining => _buildWaitingView('Thiết bị đang kết nối với mạng Wi-Fi…'),
            _Step.activating => _buildWaitingView('Đang xác nhận thiết bị…'),
            _Step.done => Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Icon(Icons.check_circle, color: DeviceDetailScreen.goodColor, size: 48),
                    const SizedBox(height: 16),
                    const Text('Ghép nối thành công!'),
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
                      onPressed: _retry,
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

  // Shared by _Step.joining/_Step.activating -- both are just a spinner +
  // status text with no interaction otherwise, so without this a device that
  // reboots mid-flow (e.g. an OTA/firmware reflash landing right in this
  // window) orphans the in-flight request and leaves the user stuck staring
  // at this for up to 120s with no way to just start over sooner.
  Widget _buildWaitingView(String message) {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          const CircularProgressIndicator(),
          const SizedBox(height: 16),
          Text(message, textAlign: TextAlign.center),
          const SizedBox(height: 24),
          OutlinedButton(
            onPressed: _cancelAndRestart,
            child: const Text('Hủy'),
          ),
        ],
      ),
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
                // Only Synora devices are ever pairable here -- everything
                // else in the scan is shown just so you can confirm BLE
                // scanning/permissions actually work, not meant to be tapped.
                return Container(
                  margin: const EdgeInsets.only(bottom: 8),
                  decoration: BoxDecoration(
                      color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(16)),
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
