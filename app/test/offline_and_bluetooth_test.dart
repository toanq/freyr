import 'dart:async';
import 'dart:io';

import 'package:connectivity_plus/connectivity_plus.dart';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:synora_app/api_client.dart';
import 'package:synora_app/common/network_errors.dart';
import 'package:synora_app/common/network_status.dart';
import 'package:synora_app/common/offline_banner.dart';
import 'package:synora_app/devices/bluetooth_state_card.dart';
import 'package:synora_app/devices/pending_pairing.dart';

class _FakePairApi extends ApiClient {
  _FakePairApi(this.failuresBeforeSuccess);
  int failuresBeforeSuccess;
  int calls = 0;

  @override
  Future<void> pairDevice(String activationCode, {String? macAddress}) async {
    calls++;
    if (failuresBeforeSuccess > 0) {
      failuresBeforeSuccess--;
      throw const NetworkException(NetworkErrorKind.offline);
    }
  }
}

Widget _app(StreamController<List<ConnectivityResult>> connectivity, {NetworkStatus? status}) {
  return ProviderScope(
    overrides: [
      networkStatusProvider.overrideWith((ref) =>
          status ??
          NetworkStatus(
            connectivityChanges: connectivity.stream,
            checkConnectivity: () async => [ConnectivityResult.wifi],
          )),
    ],
    child: MaterialApp(
      builder: (context, child) => OfflineBannerScope(child: child!),
      home: const Scaffold(body: Text('page')),
    ),
  );
}

void main() {
  group('offline banner', () {
    testWidgets('shows when connectivity drops and hides when it returns', (tester) async {
      final connectivity = StreamController<List<ConnectivityResult>>();
      await tester.pumpWidget(_app(connectivity));
      await tester.pump();
      expect(find.text(offlineBannerText), findsNothing);
      expect(find.text('page'), findsOneWidget);

      connectivity.add([ConnectivityResult.none]);
      await tester.pumpAndSettle();
      expect(find.text(offlineBannerText), findsOneWidget);
      expect(find.text('page'), findsOneWidget);

      connectivity.add([ConnectivityResult.mobile]);
      await tester.pumpAndSettle();
      expect(find.text(offlineBannerText), findsNothing);
      await connectivity.close();
    });

    testWidgets('request failures mark offline until the probe succeeds', (tester) async {
      final connectivity = StreamController<List<ConnectivityResult>>();
      var probeOk = false;
      final status = NetworkStatus(
        connectivityChanges: connectivity.stream,
        checkConnectivity: () async => [ConnectivityResult.wifi],
        probe: () async => probeOk,
        probeInterval: const Duration(seconds: 10),
      );
      await tester.pumpWidget(_app(connectivity, status: status));
      await tester.pump();

      status.reportTransportResult(ok: false);
      await tester.pumpAndSettle();
      expect(find.text(offlineBannerText), findsOneWidget);

      await tester.pump(const Duration(seconds: 11)); // probe fails, stays offline
      await tester.pumpAndSettle();
      expect(find.text(offlineBannerText), findsOneWidget);

      probeOk = true;
      await tester.pump(const Duration(seconds: 10));
      await tester.pumpAndSettle();
      expect(find.text(offlineBannerText), findsNothing);
      await connectivity.close();
    });
  });

  group('network errors', () {
    test('transport exceptions map to Vietnamese messages', () {
      expect(friendlyError(const SocketException('Failed host lookup')), 'Không có kết nối mạng');
      expect(friendlyError(http.ClientException('Connection reset')), 'Không có kết nối mạng');
      expect(friendlyError(TimeoutException('slow')), 'Máy chủ không phản hồi, thử lại sau');
      expect(friendlyError(ApiException(409, 'conflict')), contains('409'));
    });

    test('ApiClient fails fast without firing a request when there is no interface', () async {
      final reported = <bool>[];
      final api = ApiClient(
        baseUrl: 'https://example.invalid/api',
        isDefinitelyOffline: () => true,
        onTransportResult: ({required bool ok}) => reported.add(ok),
      );
      await expectLater(api.listDevices(), throwsA(isA<NetworkException>()));
      expect(reported, isEmpty);
    });
  });

  group('pending pairing', () {
    test('waits while offline and retries when the network returns', () async {
      final api = _FakePairApi(1);
      final online = StreamController<bool>();
      final pending = PendingPairing(
        pair: (code, mac) => api.pairDevice(code, macAddress: mac),
        code: 'ABC123',
        macAddress: 'AA:BB:CC:DD:EE:FF',
      );

      final result = pending.completeWhenOnline(online.stream, onlineNow: false);
      await Future<void>.delayed(Duration.zero);
      expect(api.calls, 0, reason: 'no request while known offline');

      online.add(false);
      await Future<void>.delayed(Duration.zero);
      expect(api.calls, 0);

      online.add(true); // network flaps: still fails
      await Future<void>.delayed(Duration.zero);
      expect(api.calls, 1);

      online.add(true); // back for real
      expect(await result, isTrue);
      expect(api.calls, 2);
      await online.close();
    });

    test('server errors are not swallowed', () async {
      final pending = PendingPairing(
        pair: (code, mac) async => throw ApiException(404, 'not found'),
        code: 'X',
      );
      await expectLater(pending.attempt(online: true), throwsA(isA<ApiException>()));
    });
  });

  group('Bluetooth state card', () {
    Widget card(TargetPlatform platform, BluetoothAdapterState state) => MaterialApp(
          theme: ThemeData(platform: platform),
          home: Scaffold(
            body: BluetoothStateCard(state: state, onTurnOn: () {}, onOpenSettings: () {}),
          ),
        );

    testWidgets('Android: off offers to turn Bluetooth on', (tester) async {
      await tester.pumpWidget(card(TargetPlatform.android, BluetoothAdapterState.off));
      expect(find.text('Bluetooth đang tắt'), findsOneWidget);
      expect(find.text('Bật Bluetooth'), findsOneWidget);
      expect(find.text('Mở cài đặt'), findsNothing);
    });

    testWidgets('iOS: off explains how and offers settings, no toggle', (tester) async {
      await tester.pumpWidget(card(TargetPlatform.iOS, BluetoothAdapterState.off));
      expect(find.text('Bluetooth đang tắt'), findsOneWidget);
      expect(find.text('Bật Bluetooth'), findsNothing);
      expect(find.text('Mở cài đặt'), findsOneWidget);
      expect(find.textContaining('Trung tâm điều khiển'), findsOneWidget);
    });

    testWidgets('unauthorized points to settings', (tester) async {
      await tester.pumpWidget(card(TargetPlatform.android, BluetoothAdapterState.unauthorized));
      expect(find.text('Chưa cấp quyền Bluetooth'), findsOneWidget);
      expect(find.text('Mở cài đặt'), findsOneWidget);
    });

    testWidgets('interrupted view only allows retry once Bluetooth is back', (tester) async {
      var retried = 0;
      Widget view(bool on) => MaterialApp(
            home: Scaffold(
              body: BluetoothInterruptedView(message: 'm', bluetoothOn: on, onRetry: () => retried++),
            ),
          );
      await tester.pumpWidget(view(false));
      await tester.tap(find.text('Chờ bật Bluetooth…'));
      expect(retried, 0);
      await tester.pumpWidget(view(true));
      await tester.tap(find.text('Thử lại'));
      expect(retried, 1);
    });
  });
}
