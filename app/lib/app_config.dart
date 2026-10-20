import 'package:flutter/services.dart';

// Used when the platform doesn't supply a valid value (tests, desktop, or a
// build whose manifest/Info.plist entry is missing).
const defaultApiBaseUrl = 'https://synora.id.vn/api';

// The API base URL is platform build metadata, not a Dart constant, so the
// domain can change without a code change: Android reads the
// `synora.api_base_url` <meta-data> (gradle property `synoraApiBaseUrl`),
// iOS the `SynoraApiBaseUrl` Info.plist key (ios/Flutter/Synora.xcconfig).
// See app/README.md.
const _channel = MethodChannel('synora/config');

Future<String> loadApiBaseUrl() async {
  try {
    final value = await _channel.invokeMethod<String>('apiBaseUrl');
    return normalizeApiBaseUrl(value) ?? defaultApiBaseUrl;
  } catch (_) {
    // MissingPluginException (no channel on this platform) or a platform error.
    return defaultApiBaseUrl;
  }
}

// Absolute https URL with no trailing slash, or null if unusable.
String? normalizeApiBaseUrl(String? raw) {
  if (raw == null) return null;
  var value = raw.trim();
  while (value.endsWith('/')) {
    value = value.substring(0, value.length - 1);
  }
  final uri = Uri.tryParse(value);
  if (uri == null || uri.scheme != 'https' || uri.host.isEmpty) return null;
  return value;
}
