import 'dart:convert';
import 'dart:io';

import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:http/http.dart' as http;
import 'package:http_parser/http_parser.dart';

// Carries the HTTP status code so callers can react to specific statuses
// (e.g. 409 "already paired") instead of only having a generic error string.
class ApiException implements Exception {
  ApiException(this.statusCode, this.body);
  final int statusCode;
  final String body;

  @override
  String toString() => 'Request failed (HTTP $statusCode): $body';
}

class ApiClient {
  // Briefly switched to a direct IP + plain HTTP (116.106.179.76) to work
  // around this device's recurring DNS failures -- reverted after confirming
  // live that the ESP32's own Wi-Fi network blocks outbound port 80 entirely
  // (every connect attempt failed right after boot), even though a phone on
  // cellular could reach the same address fine. Port 443 via the domain is
  // far more likely to pass through a router/ISP that specifically blocks 80.
  ApiClient({this.baseUrl = 'https://synora.carcise.dev/api'});

  // The backend only listens behind Traefik (docker-compose.yml), which strips
  // `/api` before forwarding — same convention the web console uses (VITE_API_URL=/api).
  final String baseUrl;
  final _storage = const FlutterSecureStorage();

  // http's calls have no default timeout at all -- a stalled connection (Wi-Fi
  // handoff, tunnel blip) would otherwise hang the UI (e.g. pull-to-refresh)
  // indefinitely instead of surfacing a bounded, retryable error.
  static const _timeout = Duration(seconds: 15);

  Future<String?> _token() => _storage.read(key: 'jwt');

  Future<Map<String, String>> _authHeaders() async {
    final token = await _token();
    return {
      'Content-Type': 'application/json',
      if (token != null) 'Authorization': 'Bearer $token',
    };
  }

  Future<void> register(String username, String? email, String password, {bool rememberMe = true}) async {
    final res = await http
        .post(
          Uri.parse('$baseUrl/auth/register'),
          headers: {'Content-Type': 'application/json'},
          body: jsonEncode({'username': username, 'email': email, 'password': password}),
        )
        .timeout(_timeout);
    _throwIfError(res);
    await _storeToken(res, rememberMe: rememberMe);
  }

  Future<void> login(String username, String password, {bool rememberMe = true}) async {
    final res = await http
        .post(
          Uri.parse('$baseUrl/auth/token'),
          headers: {'Content-Type': 'application/json'},
          body: jsonEncode({'username': username, 'password': password}),
        )
        .timeout(_timeout);
    _throwIfError(res);
    await _storeToken(res, rememberMe: rememberMe);
  }

  Future<void> loginWithGoogle(String idToken) async {
    final res = await http
        .post(
          Uri.parse('$baseUrl/auth/oauth/google'),
          headers: {'Content-Type': 'application/json'},
          body: jsonEncode({'idToken': idToken}),
        )
        .timeout(_timeout);
    _throwIfError(res);
    await _storeToken(res, rememberMe: true);
  }

  Future<void> loginWithFacebook(String accessToken) async {
    final res = await http
        .post(
          Uri.parse('$baseUrl/auth/oauth/facebook'),
          headers: {'Content-Type': 'application/json'},
          body: jsonEncode({'accessToken': accessToken}),
        )
        .timeout(_timeout);
    _throwIfError(res);
    await _storeToken(res, rememberMe: true);
  }

  Future<void> logout() async {
    await _storage.delete(key: 'jwt');
    await _storage.delete(key: 'remember_me');
  }

  // Whether a token was stored with "remember me" checked — used at app
  // startup to decide whether to skip straight past the login screen.
  Future<bool> hasRememberedSession() async {
    final remembered = await _storage.read(key: 'remember_me');
    final token = await _token();
    return remembered == 'true' && token != null;
  }

  Future<List<Map<String, dynamic>>> listDevices() async {
    final res = await http.get(Uri.parse('$baseUrl/devices'), headers: await _authHeaders()).timeout(_timeout);
    _throwIfError(res);
    return List<Map<String, dynamic>>.from(jsonDecode(res.body) as List);
  }

  Future<void> pairDevice(String activationCode, {String? macAddress}) async {
    final res = await http
        .post(
          Uri.parse('$baseUrl/devices/pair'),
          headers: await _authHeaders(),
          body: jsonEncode({'activationCode': activationCode, 'macAddress': macAddress}),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<void> triggerManualWatering(String deviceId, int durationSeconds) async {
    final res = await http
        .post(
          Uri.parse('$baseUrl/devices/$deviceId/water'),
          headers: await _authHeaders(),
          body: jsonEncode({'durationSeconds': durationSeconds}),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<void> pingDevice(String deviceId) async {
    final res = await http
        .post(
          Uri.parse('$baseUrl/devices/$deviceId/ping'),
          headers: await _authHeaders(),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<void> setDeviceMuted(String deviceId, bool muted) async {
    final res = await http
        .put(
          Uri.parse('$baseUrl/devices/$deviceId/muted'),
          headers: await _authHeaders(),
          body: jsonEncode({'muted': muted}),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  // Registers/updates this installation's FCM push token against the signed-in
  // user, so an admin can send it a test notification from the web console.
  Future<void> registerFcmToken(String fcmToken) async {
    final res = await http
        .put(
          Uri.parse('$baseUrl/users/me/fcm-token'),
          headers: await _authHeaders(),
          body: jsonEncode({'token': fcmToken}),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<List<Map<String, dynamic>>> listWateringSchedules(String deviceId) async {
    final res = await http
        .get(Uri.parse('$baseUrl/devices/$deviceId/schedules'), headers: await _authHeaders())
        .timeout(_timeout);
    _throwIfError(res);
    return List<Map<String, dynamic>>.from(jsonDecode(res.body) as List);
  }

  Future<void> createWateringSchedule(String deviceId, {required int hour, required int minute, required int durationSeconds}) async {
    final res = await http
        .post(
          Uri.parse('$baseUrl/devices/$deviceId/schedules'),
          headers: await _authHeaders(),
          body: jsonEncode({'hour': hour, 'minute': minute, 'durationSeconds': durationSeconds}),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<void> updateWateringSchedule(
    String deviceId,
    String scheduleId, {
    required int hour,
    required int minute,
    required int durationSeconds,
    required bool enabled,
  }) async {
    final res = await http
        .patch(
          Uri.parse('$baseUrl/devices/$deviceId/schedules/$scheduleId'),
          headers: await _authHeaders(),
          body: jsonEncode({'hour': hour, 'minute': minute, 'durationSeconds': durationSeconds, 'enabled': enabled}),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<void> deleteWateringSchedule(String deviceId, String scheduleId) async {
    final res = await http
        .delete(Uri.parse('$baseUrl/devices/$deviceId/schedules/$scheduleId'), headers: await _authHeaders())
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<Map<String, dynamic>> getDevice(String deviceId) async {
    final res =
        await http.get(Uri.parse('$baseUrl/devices/$deviceId'), headers: await _authHeaders()).timeout(_timeout);
    _throwIfError(res);
    return jsonDecode(res.body) as Map<String, dynamic>;
  }

  Future<void> renameDevice(String deviceId, String name) async {
    final res = await http
        .patch(
          Uri.parse('$baseUrl/devices/$deviceId'),
          headers: await _authHeaders(),
          body: jsonEncode({'name': name}),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<void> deleteDevice(String deviceId) async {
    final res =
        await http.delete(Uri.parse('$baseUrl/devices/$deviceId'), headers: await _authHeaders()).timeout(_timeout);
    _throwIfError(res);
  }

  // warningPercent: in-app-only (icon + yellow highlight), no push. Pass null
  // to disable. criticalPercent: sends a push notification; must be <=
  // warningPercent whenever both are set. Pass null to disable.
  // intervalMinutes null means alert once per drop below the critical
  // threshold; otherwise it re-notifies every intervalMinutes while still at
  // or below it.
  Future<void> updateLowWaterAlerts(
    String deviceId, {
    int? warningPercent,
    int? criticalPercent,
    int? intervalMinutes,
  }) async {
    final res = await http
        .patch(
          Uri.parse('$baseUrl/devices/$deviceId/alerts'),
          headers: await _authHeaders(),
          body: jsonEncode({
            'lowWaterWarningThresholdPercent': warningPercent,
            'lowWaterCriticalThresholdPercent': criticalPercent,
            'lowWaterAlertIntervalMinutes': intervalMinutes,
          }),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<Map<String, dynamic>> getMyProfile() async {
    final res = await http.get(Uri.parse('$baseUrl/users/me'), headers: await _authHeaders()).timeout(_timeout);
    _throwIfError(res);
    return jsonDecode(res.body) as Map<String, dynamic>;
  }

  Future<void> updateMyEmail(String? email) async {
    final res = await http
        .patch(Uri.parse('$baseUrl/users/me'), headers: await _authHeaders(), body: jsonEncode({'email': email}))
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<void> changeMyPassword(String currentPassword, String newPassword) async {
    final res = await http
        .patch(
          Uri.parse('$baseUrl/users/me/password'),
          headers: await _authHeaders(),
          body: jsonEncode({'currentPassword': currentPassword, 'newPassword': newPassword}),
        )
        .timeout(_timeout);
    _throwIfError(res);
  }

  Future<String> uploadMyAvatar(File imageFile) async {
    final token = await _token();
    final request = http.MultipartRequest('POST', Uri.parse('$baseUrl/users/me/avatar'))
      ..headers.addAll({if (token != null) 'Authorization': 'Bearer $token'})
      ..files.add(await http.MultipartFile.fromPath('file', imageFile.path, contentType: _mediaTypeFor(imageFile.path)));
    final streamedRes = await request.send().timeout(const Duration(seconds: 30));
    final res = await http.Response.fromStream(streamedRes);
    _throwIfError(res);
    return (jsonDecode(res.body) as Map<String, dynamic>)['avatarUrl'] as String;
  }

  Future<String> uploadDeviceAvatar(String deviceId, File imageFile) async {
    final token = await _token();
    final request = http.MultipartRequest('POST', Uri.parse('$baseUrl/devices/$deviceId/avatar'))
      ..headers.addAll({if (token != null) 'Authorization': 'Bearer $token'})
      ..files.add(await http.MultipartFile.fromPath('file', imageFile.path, contentType: _mediaTypeFor(imageFile.path)));
    // Longer timeout than _timeout -- this uploads a multi-MB file, unlike the
    // small JSON payloads everywhere else.
    final streamedRes = await request.send().timeout(const Duration(seconds: 30));
    final res = await http.Response.fromStream(streamedRes);
    _throwIfError(res);
    return (jsonDecode(res.body) as Map<String, dynamic>)['avatarUrl'] as String;
  }

  MediaType _mediaTypeFor(String path) {
    final lower = path.toLowerCase();
    if (lower.endsWith('.png')) return MediaType('image', 'png');
    if (lower.endsWith('.webp')) return MediaType('image', 'webp');
    return MediaType('image', 'jpeg');
  }

  // Role of the signed-in user ("User", "Admin", "SuperAdmin") -- used to hide
  // the owner-name chip in the device list for regular users, since it's
  // meaningless there (they only ever see their own devices).
  Future<String?> currentUserRole() async {
    final res = await http.get(Uri.parse('$baseUrl/auth/session'), headers: await _authHeaders()).timeout(_timeout);
    _throwIfError(res);
    final body = jsonDecode(res.body) as Map<String, dynamic>;
    return body['role'] as String?;
  }

  Future<void> _storeToken(http.Response res, {required bool rememberMe}) async {
    final body = jsonDecode(res.body) as Map<String, dynamic>;
    await _storage.write(key: 'jwt', value: body['token'] as String);
    await _storage.write(key: 'remember_me', value: rememberMe.toString());
  }

  void _throwIfError(http.Response res) {
    if (res.statusCode >= 400) {
      throw ApiException(res.statusCode, res.body);
    }
  }
}
