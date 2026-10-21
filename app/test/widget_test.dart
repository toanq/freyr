import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:synora_app/main.dart';

void main() {
  testWidgets('Shows the login screen when logged out', (WidgetTester tester) async {
    await tester.pumpWidget(const ProviderScope(child: SynoraApp()));
    // The startup "remembered session" check has a 3s timeout (main.dart) — advance
    // past it explicitly rather than pumpAndSettle(), which never resolves against
    // an indefinitely-spinning CircularProgressIndicator.
    await tester.pump(const Duration(seconds: 4));

    expect(find.widgetWithText(FilledButton, 'Đăng nhập'), findsOneWidget);
  });
}
