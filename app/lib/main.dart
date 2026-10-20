import 'dart:async';

import 'package:firebase_core/firebase_core.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'api_client.dart';
import 'app_config.dart';
import 'auth/auth_screen.dart';
import 'devices/device_list_screen.dart';
import 'push_notifications.dart';

final apiClientProvider = Provider((ref) => ApiClient());
final isLoggedInProvider = StateProvider<bool>((ref) => false);

// Lets DeviceListScreen reload every time it becomes visible again (returning
// from BlePairingScreen, DeviceDetailScreen, or anywhere else), regardless of
// how the caller got back here -- see RouteAware in device_list_screen.dart.
final routeObserver = RouteObserver<PageRoute>();

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  try {
    await Firebase.initializeApp();
  } catch (_) {
    // Non-fatal -- push notifications are optional, the app works without them.
  }
  final apiBaseUrl = await loadApiBaseUrl();
  runApp(ProviderScope(
    overrides: [apiClientProvider.overrideWithValue(ApiClient(baseUrl: apiBaseUrl))],
    child: const SynoraApp(),
  ));
}

class SynoraApp extends ConsumerStatefulWidget {
  const SynoraApp({super.key});

  @override
  ConsumerState<SynoraApp> createState() => _SynoraAppState();
}

class _SynoraAppState extends ConsumerState<SynoraApp> {
  bool _checkingRememberedSession = true;

  @override
  void initState() {
    super.initState();
    _checkRememberedSession();
  }

  Future<void> _checkRememberedSession() async {
    try {
      final remembered = await ref
          .read(apiClientProvider)
          .hasRememberedSession()
          .timeout(const Duration(seconds: 3), onTimeout: () => false);
      if (remembered) {
        ref.read(isLoggedInProvider.notifier).state = true;
        unawaited(registerForPushNotifications(ref.read(apiClientProvider)));
      }
    } catch (_) {
      // Fall through to the login screen if the stored session can't be read.
    } finally {
      if (mounted) setState(() => _checkingRememberedSession = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final loggedIn = ref.watch(isLoggedInProvider);
    return MaterialApp(
      title: 'Synora',
      theme: ThemeData(colorSchemeSeed: Colors.teal, useMaterial3: true),
      navigatorObservers: [routeObserver],
      home: _checkingRememberedSession
          ? const Scaffold(body: Center(child: CircularProgressIndicator()))
          : (loggedIn ? const DeviceListScreen() : const AuthScreen()),
    );
  }
}
