import 'dart:async';

import '../common/network_errors.dart';

// Final step of BLE pairing (POST /devices/pair) once the device has handed
// over its activation code. If the phone is offline at that moment, the code
// is kept and the request retried when the network returns, instead of
// failing the whole pairing flow.
class PendingPairing {
  PendingPairing({required this.pair, required this.code, this.macAddress});

  final Future<void> Function(String code, String? macAddress) pair;
  final String code;
  final String? macAddress;

  // true = paired; false = still offline, try again later. Other errors
  // (ApiException etc.) propagate to the caller.
  Future<bool> attempt({required bool online}) async {
    if (!online) return false;
    try {
      await pair(code, macAddress);
      return true;
    } on NetworkException {
      return false;
    }
  }

  // Tries now, then again every time [onlineChanges] reports online, until it
  // succeeds. Completes with false if the stream ends first (screen closed).
  Future<bool> completeWhenOnline(Stream<bool> onlineChanges, {required bool onlineNow}) async {
    if (await attempt(online: onlineNow)) return true;
    await for (final online in onlineChanges) {
      if (online && await attempt(online: true)) return true;
    }
    return false;
  }
}
