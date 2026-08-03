import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import 'api_client.dart';

const _androidChannel = AndroidNotificationChannel(
  'default_channel',
  'Thông báo',
  description: 'Thông báo từ máy chủ Synora',
  importance: Importance.high,
);

final _localNotifications = FlutterLocalNotificationsPlugin();

// Requests notification permission and registers this installation's FCM
// token with the backend, so an admin can send it a test notification from
// the web console. Safe to call repeatedly (e.g. after every login); failures
// are swallowed since push notifications are optional, not required for the
// app to function.
Future<void> registerForPushNotifications(ApiClient apiClient) async {
  try {
    final messaging = FirebaseMessaging.instance;
    await messaging.requestPermission();

    // FCM notification payloads are auto-displayed by the OS only when the
    // app is backgrounded/killed -- while it's in the foreground (the common
    // case when testing from the web console), nothing shows unless the app
    // displays it itself via a local notification, hence all of this.
    await _localNotifications.initialize(
      const InitializationSettings(android: AndroidInitializationSettings('@mipmap/ic_launcher')),
    );
    await _localNotifications
        .resolvePlatformSpecificImplementation<AndroidFlutterLocalNotificationsPlugin>()
        ?.createNotificationChannel(_androidChannel);

    FirebaseMessaging.onMessage.listen((message) {
      final notification = message.notification;
      if (notification == null) return;
      _localNotifications.show(
        notification.hashCode,
        notification.title,
        notification.body,
        NotificationDetails(
          android: AndroidNotificationDetails(
            _androidChannel.id,
            _androidChannel.name,
            channelDescription: _androidChannel.description,
            importance: Importance.high,
            priority: Priority.high,
          ),
        ),
      );
    });

    final token = await messaging.getToken();
    if (token != null) {
      await apiClient.registerFcmToken(token);
    }

    messaging.onTokenRefresh.listen((newToken) {
      apiClient.registerFcmToken(newToken).catchError((_) {});
    });
  } catch (_) {
    // Non-fatal -- push notifications are optional.
  }
}
