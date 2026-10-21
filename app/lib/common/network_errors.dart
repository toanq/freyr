import 'dart:async';
import 'dart:io';

import 'package:http/http.dart' as http;

enum NetworkErrorKind { offline, serverUnreachable }

// Transport-level failure (no response at all), as opposed to ApiException,
// which means the server answered with an error status.
class NetworkException implements Exception {
  const NetworkException(this.kind);

  final NetworkErrorKind kind;

  String get message => switch (kind) {
        NetworkErrorKind.offline => 'Không có kết nối mạng',
        NetworkErrorKind.serverUnreachable => 'Máy chủ không phản hồi, thử lại sau',
      };

  @override
  String toString() => message;
}

// Classifies a raw transport exception, or returns null if it isn't one.
// SocketException/ClientException mean the phone couldn't reach anything
// (Wi-Fi/data off, DNS failure); a timeout or TLS failure means the network is
// up but the server didn't answer properly.
NetworkException? asNetworkException(Object error) {
  if (error is NetworkException) return error;
  if (error is SocketException || error is http.ClientException) {
    return const NetworkException(NetworkErrorKind.offline);
  }
  if (error is TimeoutException || error is HandshakeException) {
    return const NetworkException(NetworkErrorKind.serverUnreachable);
  }
  return null;
}

// What screens show instead of a raw `$e` -- the Vietnamese message for
// network failures, the exception's own text otherwise.
String friendlyError(Object error) => asNetworkException(error)?.message ?? error.toString();
