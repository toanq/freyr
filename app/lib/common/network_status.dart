import 'dart:async';

import 'package:connectivity_plus/connectivity_plus.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

// true = online. Two sources feed it:
//  * connectivity_plus: no network interface at all ("none") -> offline.
//  * ApiClient transport results: an interface exists but requests fail with
//    SocketException/ClientException (captive portal, router without
//    internet) -> offline until a request succeeds. While in that state a
//    probe runs every [probeInterval] so the banner clears on its own.
class NetworkStatus extends StateNotifier<bool> {
  NetworkStatus({
    required Stream<List<ConnectivityResult>> connectivityChanges,
    required Future<List<ConnectivityResult>> Function() checkConnectivity,
    Future<bool> Function()? probe,
    this.probeInterval = const Duration(seconds: 15),
  })  : _probe = probe,
        super(true) {
    // onError: no plugin (tests/desktop) or a platform hiccup -- stay online.
    _sub = connectivityChanges.listen(_onConnectivity, onError: (_) {});
    checkConnectivity().then(_onConnectivity).catchError((_) {});
  }

  final Future<bool> Function()? _probe;
  final Duration probeInterval;
  StreamSubscription<List<ConnectivityResult>>? _sub;
  Timer? _probeTimer;
  bool _hasInterface = true;
  bool _requestsFailing = false;

  // No interface at all: requests can't possibly work, so ApiClient fails
  // them immediately instead of waiting for a timeout.
  bool get hasNoInterface => !_hasInterface;

  void _onConnectivity(List<ConnectivityResult> results) {
    _hasInterface = results.any((r) => r != ConnectivityResult.none);
    // A fresh interface (Wi-Fi back, switched to data) deserves a fresh
    // chance: forget earlier request failures and let the next call decide.
    if (_hasInterface) _requestsFailing = false;
    _update();
  }

  void reportTransportResult({required bool ok}) {
    if (ok == !_requestsFailing) return;
    _requestsFailing = !ok;
    _update();
  }

  void _update() {
    final online = _hasInterface && !_requestsFailing;
    if (mounted && state != online) state = online;
    if (_hasInterface && _requestsFailing && _probe != null) {
      _probeTimer ??= Timer.periodic(probeInterval, (_) => _runProbe());
    } else {
      _probeTimer?.cancel();
      _probeTimer = null;
    }
  }

  Future<void> _runProbe() async {
    try {
      final ok = await _probe!();
      if (ok) reportTransportResult(ok: true);
    } catch (_) {
      // Still failing; next tick tries again.
    }
  }

  @override
  void dispose() {
    _sub?.cancel();
    _probeTimer?.cancel();
    super.dispose();
  }
}

// Overridden in main() so the probe can use the configured ApiClient, and in
// tests with fake streams.
final networkStatusProvider = StateNotifierProvider<NetworkStatus, bool>((ref) {
  final connectivity = Connectivity();
  return NetworkStatus(
    connectivityChanges: connectivity.onConnectivityChanged,
    checkConnectivity: connectivity.checkConnectivity,
  );
});
