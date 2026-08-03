import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:image_picker/image_picker.dart';

import '../devices/device_detail_screen.dart';
import '../main.dart';

class UserProfileScreen extends ConsumerStatefulWidget {
  const UserProfileScreen({super.key});

  @override
  ConsumerState<UserProfileScreen> createState() => _UserProfileScreenState();
}

class _UserProfileScreenState extends ConsumerState<UserProfileScreen> {
  Map<String, dynamic>? _profile;
  bool _uploadingAvatar = false;

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _reload() async {
    try {
      final profile = await ref.read(apiClientProvider).getMyProfile();
      if (mounted) setState(() => _profile = profile);
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể tải hồ sơ: $e')));
      }
    }
  }

  Future<void> _pickAndUploadAvatar() async {
    final source = await showModalBottomSheet<ImageSource>(
      context: context,
      builder: (context) => SafeArea(
        child: Wrap(
          children: [
            ListTile(
              leading: const Icon(Icons.camera_alt),
              title: const Text('Chụp ảnh'),
              onTap: () => Navigator.of(context).pop(ImageSource.camera),
            ),
            ListTile(
              leading: const Icon(Icons.photo_library),
              title: const Text('Chọn từ thư viện'),
              onTap: () => Navigator.of(context).pop(ImageSource.gallery),
            ),
          ],
        ),
      ),
    );
    if (source == null) return;

    final picked = await ImagePicker().pickImage(source: source, maxWidth: 1024, imageQuality: 85);
    if (picked == null) return;

    setState(() => _uploadingAvatar = true);
    try {
      final avatarUrl = await ref.read(apiClientProvider).uploadMyAvatar(File(picked.path));
      if (mounted) {
        // avatarUrl is the same path every time ("/users/{id}/avatar"), so
        // NetworkImage's cache (keyed by URL) would keep showing the old
        // bytes forever without this -- the upload itself already succeeds
        // server-side, this is purely a client-side stale-cache issue.
        final baseUrl = ref.read(apiClientProvider).baseUrl;
        PaintingBinding.instance.imageCache.evict(NetworkImage('$baseUrl$avatarUrl'));
        setState(() => _profile = {...?_profile, 'avatarUrl': avatarUrl});
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể tải ảnh lên: $e')));
      }
    } finally {
      if (mounted) setState(() => _uploadingAvatar = false);
    }
  }

  Future<void> _editEmail() async {
    final controller = TextEditingController(text: _profile?['email'] as String? ?? '');
    final newEmail = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Cập nhật email'),
        content: TextField(
          controller: controller,
          autofocus: true,
          keyboardType: TextInputType.emailAddress,
          decoration: const InputDecoration(labelText: 'Email'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(), child: const Text('Hủy')),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(controller.text.trim()),
            child: const Text('Lưu'),
          ),
        ],
      ),
    );
    if (newEmail == null) return;

    try {
      await ref.read(apiClientProvider).updateMyEmail(newEmail.isEmpty ? null : newEmail);
      if (mounted) setState(() => _profile = {...?_profile, 'email': newEmail.isEmpty ? null : newEmail});
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể cập nhật email: $e')));
      }
    }
  }

  Future<void> _changePassword() async {
    final currentController = TextEditingController();
    final newController = TextEditingController();
    bool obscureCurrent = true;
    bool obscureNew = true;
    String? error;

    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => StatefulBuilder(
        builder: (context, setDialogState) => AlertDialog(
          title: const Text('Đổi mật khẩu'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              TextField(
                controller: currentController,
                autofocus: true,
                obscureText: obscureCurrent,
                decoration: InputDecoration(
                  labelText: 'Mật khẩu hiện tại',
                  suffixIcon: IconButton(
                    icon: Icon(obscureCurrent ? Icons.visibility_off : Icons.visibility),
                    onPressed: () => setDialogState(() => obscureCurrent = !obscureCurrent),
                  ),
                ),
              ),
              const SizedBox(height: 12),
              TextField(
                controller: newController,
                obscureText: obscureNew,
                decoration: InputDecoration(
                  labelText: 'Mật khẩu mới',
                  suffixIcon: IconButton(
                    icon: Icon(obscureNew ? Icons.visibility_off : Icons.visibility),
                    onPressed: () => setDialogState(() => obscureNew = !obscureNew),
                  ),
                ),
              ),
              if (error != null) ...[
                const SizedBox(height: 12),
                Text(error!, style: TextStyle(color: Theme.of(context).colorScheme.error)),
              ],
            ],
          ),
          actions: [
            TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Hủy')),
            FilledButton(
              onPressed: () {
                if (newController.text.length < 6) {
                  setDialogState(() => error = 'Mật khẩu mới phải có ít nhất 6 ký tự.');
                  return;
                }
                Navigator.of(context).pop(true);
              },
              child: const Text('Lưu'),
            ),
          ],
        ),
      ),
    );
    if (confirmed != true || !mounted) return;

    try {
      await ref.read(apiClientProvider).changeMyPassword(currentController.text, newController.text);
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Đã đổi mật khẩu.')));
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Không thể đổi mật khẩu: $e')));
      }
    }
  }

  Future<void> _logout() async {
    await ref.read(apiClientProvider).logout();
    ref.read(isLoggedInProvider.notifier).state = false;
    // This screen is pushed on top of DeviceListScreen -- flipping the
    // provider alone only affects MaterialApp's root (home), which is
    // covered by this still-pushed route, so nothing visibly changes
    // without popping back to it.
    if (mounted) Navigator.of(context).popUntil((route) => route.isFirst);
  }

  @override
  Widget build(BuildContext context) {
    final baseUrl = ref.read(apiClientProvider).baseUrl;
    final avatarUrl = _profile?['avatarUrl'] as String?;
    final username = _profile?['username'] as String? ?? '';
    final email = _profile?['email'] as String?;

    return Scaffold(
      backgroundColor: DeviceDetailScreen.backgroundColor,
      appBar: AppBar(
        backgroundColor: DeviceDetailScreen.backgroundColor,
        foregroundColor: Colors.white,
        title: const Text('Tài khoản'),
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.fromLTRB(20, 24, 20, 24),
          children: [
            Center(
              child: GestureDetector(
                onTap: _uploadingAvatar ? null : _pickAndUploadAvatar,
                child: Container(
                  width: 120,
                  height: 120,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: const Color(0xFF163826),
                    border: Border.all(color: const Color(0xFF2E5A3E), width: 3),
                    image: avatarUrl != null
                        ? DecorationImage(image: NetworkImage('$baseUrl$avatarUrl'), fit: BoxFit.cover)
                        : null,
                  ),
                  child: _uploadingAvatar
                      ? const Center(child: CircularProgressIndicator(color: DeviceDetailScreen.goodColor))
                      : avatarUrl == null
                          ? const Icon(Icons.person, size: 56, color: DeviceDetailScreen.goodColor)
                          : Align(
                              alignment: Alignment.bottomRight,
                              child: Container(
                                margin: const EdgeInsets.all(6),
                                padding: const EdgeInsets.all(6),
                                decoration: const BoxDecoration(color: Colors.black54, shape: BoxShape.circle),
                                child: const Icon(Icons.camera_alt, size: 16, color: Colors.white),
                              ),
                            ),
                ),
              ),
            ),
            const SizedBox(height: 12),
            Center(
              child: Text(username,
                  style: const TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.bold)),
            ),
            const SizedBox(height: 24),
            Container(
              decoration:
                  BoxDecoration(color: DeviceDetailScreen.cardColor, borderRadius: BorderRadius.circular(20)),
              child: Column(
                children: [
                  ListTile(
                    leading: const Icon(Icons.email_outlined, color: Colors.white70),
                    title: const Text('Email', style: TextStyle(color: Colors.white70, fontSize: 12)),
                    subtitle: Text(email ?? 'Chưa đặt', style: const TextStyle(color: Colors.white)),
                    trailing: const Icon(Icons.chevron_right, color: Colors.white38),
                    onTap: _editEmail,
                  ),
                  const Divider(height: 1, color: Color(0xFF2E5A3E)),
                  ListTile(
                    leading: const Icon(Icons.lock_outline, color: Colors.white70),
                    title: const Text('Đổi mật khẩu', style: TextStyle(color: Colors.white)),
                    trailing: const Icon(Icons.chevron_right, color: Colors.white38),
                    onTap: _changePassword,
                  ),
                ],
              ),
            ),
            const SizedBox(height: 24),
            OutlinedButton.icon(
              style: OutlinedButton.styleFrom(foregroundColor: DeviceDetailScreen.lowColor),
              onPressed: _logout,
              icon: const Icon(Icons.logout),
              label: const Text('Đăng xuất'),
            ),
          ],
        ),
      ),
    );
  }
}
