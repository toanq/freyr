import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../common/network_errors.dart';
import '../devices/device_detail_screen.dart';
import '../main.dart';
import '../push_notifications.dart';

class AuthScreen extends ConsumerStatefulWidget {
  const AuthScreen({super.key});

  @override
  ConsumerState<AuthScreen> createState() => _AuthScreenState();
}

class _AuthScreenState extends ConsumerState<AuthScreen> {
  final _usernameController = TextEditingController();
  final _emailController = TextEditingController();
  final _passwordController = TextEditingController();
  bool _isRegistering = false;
  bool _submitting = false;
  bool _rememberMe = true;
  bool _obscurePassword = true;
  String? _error;

  Future<void> _submit() async {
    setState(() {
      _submitting = true;
      _error = null;
    });
    try {
      final api = ref.read(apiClientProvider);
      if (_isRegistering) {
        await api.register(
          _usernameController.text,
          _emailController.text.isEmpty ? null : _emailController.text,
          _passwordController.text,
          rememberMe: _rememberMe,
        );
      } else {
        await api.login(_usernameController.text, _passwordController.text, rememberMe: _rememberMe);
      }
      ref.read(isLoggedInProvider.notifier).state = true;
      unawaited(registerForPushNotifications(api));
    } catch (e) {
      setState(() => _error = friendlyError(e));
    } finally {
      setState(() => _submitting = false);
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
        title: const Text('Synora'),
      ),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: ListView(
          children: [
            Image.asset('assets/icon/icon.png', height: 96),
            const SizedBox(height: 24),
            TextField(
              controller: _usernameController,
              style: const TextStyle(color: Colors.white),
              decoration: _fieldDecoration('Tên đăng nhập'),
            ),
            if (_isRegistering) ...[
              const SizedBox(height: 12),
              TextField(
                controller: _emailController,
                style: const TextStyle(color: Colors.white),
                decoration: _fieldDecoration('Email (không bắt buộc)'),
              ),
            ],
            const SizedBox(height: 12),
            TextField(
              controller: _passwordController,
              style: const TextStyle(color: Colors.white),
              decoration: _fieldDecoration('Mật khẩu').copyWith(
                suffixIcon: IconButton(
                  icon: Icon(_obscurePassword ? Icons.visibility_off : Icons.visibility, color: Colors.white70),
                  onPressed: () => setState(() => _obscurePassword = !_obscurePassword),
                ),
              ),
              obscureText: _obscurePassword,
            ),
            CheckboxListTile(
              value: _rememberMe,
              onChanged: (value) => setState(() => _rememberMe = value ?? true),
              title: const Text('Ghi nhớ đăng nhập', style: TextStyle(color: Colors.white)),
              activeColor: DeviceDetailScreen.goodColor,
              controlAffinity: ListTileControlAffinity.leading,
              contentPadding: EdgeInsets.zero,
            ),
            const SizedBox(height: 8),
            FilledButton(
              style: FilledButton.styleFrom(
                backgroundColor: DeviceDetailScreen.goodColor,
                foregroundColor: DeviceDetailScreen.backgroundColor,
              ),
              onPressed: _submitting ? null : _submit,
              child: Text(_isRegistering ? 'Tạo tài khoản' : 'Đăng nhập'),
            ),
            TextButton(
              onPressed: () => setState(() => _isRegistering = !_isRegistering),
              child: Text(_isRegistering ? 'Đã có tài khoản? Đăng nhập' : 'Chưa có tài khoản? Tạo tài khoản mới',
                  style: const TextStyle(color: DeviceDetailScreen.highColor)),
            ),
            if (_error != null) ...[
              const SizedBox(height: 16),
              Text(_error!, style: const TextStyle(color: DeviceDetailScreen.lowColor)),
            ],
          ],
        ),
      ),
    );
  }
}
