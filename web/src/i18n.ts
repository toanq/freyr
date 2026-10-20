import { createContext, useContext } from 'react'

export type Lang = 'vi' | 'en'

// Vietnamese wording follows the mobile app's existing strings (app/lib/devices/*).
export const strings = {
  // Login
  'login.subtitle': { vi: 'Đăng nhập để chăm sóc cây của bạn', en: 'Sign in to manage your plants' },
  'login.username': { vi: 'Tên đăng nhập', en: 'Username' },
  'login.password': { vi: 'Mật khẩu', en: 'Password' },
  'login.submit': { vi: 'Đăng nhập', en: 'Sign in' },
  'login.submitting': { vi: 'Đang đăng nhập…', en: 'Signing in…' },
  'login.invalid': { vi: 'Sai tên đăng nhập hoặc mật khẩu.', en: 'Invalid username or password.' },
  'login.notConfigured': { vi: 'Máy chủ chưa cấu hình đăng nhập.', en: 'Login is not configured on the server.' },
  'login.unreachable': { vi: 'Không kết nối được máy chủ ({error})', en: 'Could not reach the server ({error})' },

  // Shell
  'shell.signOut': { vi: 'Đăng xuất', en: 'Sign out' },
  'shell.language': { vi: 'Ngôn ngữ', en: 'Language' },
  'tab.devices': { vi: 'Thiết bị', en: 'Devices' },
  'tab.firmware': { vi: 'Firmware', en: 'Firmware' },
  'tab.status': { vi: 'Trạng thái', en: 'Status' },
  'tab.users': { vi: 'Người dùng', en: 'Users' },
  'tab.adminTest': { vi: 'Kiểm thử', en: 'Admin test' },
  'tab.duplicates': { vi: 'Trùng lặp', en: 'Duplicates' },
  'tab.micTests': { vi: 'Ghi âm mic', en: 'Mic tests' },
  'tab.systemConfig': { vi: 'Cấu hình', en: 'System config' },

  // Devices
  'devices.all': { vi: 'Tất cả thiết bị', en: 'All devices' },
  'devices.mine': { vi: 'Thiết bị của tôi', en: 'My devices' },
  'devices.activationCode': { vi: 'Mã kích hoạt', en: 'Activation code' },
  'devices.pair': { vi: 'Ghép nối thiết bị', en: 'Pair device' },
  'devices.emptyAdmin': { vi: 'Chưa có thiết bị nào được ghép nối.', en: 'No paired devices yet.' },
  'devices.emptyUser': {
    vi: 'Chưa có thiết bị nào. Hãy ghép nối bằng ứng dụng Synora trên điện thoại.',
    en: 'No devices yet. Pair one with the Synora mobile app.',
  },
  'devices.loadFailed': { vi: 'Không thể tải danh sách thiết bị.', en: 'Could not load devices.' },
  'devices.alreadyPaired': { vi: 'Thiết bị đã được ghép nối.', en: 'Device already paired.' },
  'devices.unknownCode': { vi: 'Mã kích hoạt không hợp lệ.', en: 'Unknown activation code.' },
  'devices.pairFailed': { vi: 'Không thể ghép nối thiết bị (HTTP {status}).', en: 'Could not pair device (HTTP {status}).' },
  'devices.renameFailed': { vi: 'Không thể đổi tên thiết bị (HTTP {status}).', en: 'Could not rename device (HTTP {status}).' },
  'devices.removeFailed': { vi: 'Không thể xoá thiết bị (HTTP {status}).', en: 'Could not remove device (HTTP {status}).' },
  'devices.muteFailed': {
    vi: 'Không thể cập nhật chế độ tắt tiếng (HTTP {status}).',
    en: 'Could not update muted state (HTTP {status}).',
  },
  'devices.unpaired': {
    vi: 'Đã huỷ ghép nối {name}. Thiết bị vẫn hiển thị trong "Tất cả thiết bị" (không có chủ) để ghép nối lại.',
    en: '{name} unpaired. It stays listed under "All devices" (owner cleared) so it can be re-paired.',
  },
  'devices.removed': { vi: 'Đã xoá {name} khỏi thiết bị của bạn.', en: '{name} removed from your devices.' },
  'devices.defaultName': { vi: 'Thiết bị', en: 'Device' },
  'devices.nameLabel': { vi: 'Tên thiết bị', en: 'Device name' },
  'devices.noOwner': { vi: 'chưa ghép', en: 'unpaired' },
  'devices.online': { vi: 'Trực tuyến', en: 'Online' },
  'devices.offline': { vi: 'Ngoại tuyến', en: 'Offline' },
  'devices.muted': { vi: 'Đã tắt tiếng', en: 'Muted' },
  'devices.notPaired': { vi: 'Chưa ghép nối', en: 'Not paired' },
  'devices.pairedAt': { vi: 'Ghép nối {date}', en: 'Paired {date}' },
  'devices.conversations': { vi: 'Hội thoại', en: 'Conversations' },
  'devices.hideConversations': { vi: 'Ẩn hội thoại', en: 'Hide conversations' },
  'devices.mute': { vi: 'Tắt tiếng', en: 'Mute' },
  'devices.unmute': { vi: 'Bật tiếng', en: 'Unmute' },
  'devices.testReading': { vi: 'Gửi dữ liệu thử', en: 'Test reading' },
  'devices.unpair': { vi: 'Huỷ ghép nối', en: 'Unpair' },
  'devices.remove': { vi: 'Xoá', en: 'Remove' },
  'devices.unpairHint': {
    vi: 'Huỷ ghép nối và xoá chủ sở hữu -- thiết bị vẫn ở trong danh sách để ghép nối lại.',
    en: 'Unpairs the device and clears its owner -- it stays in this list, ready to be re-paired.',
  },
  'devices.removeHint': { vi: 'Xoá thiết bị này khỏi tài khoản của bạn.', en: 'Removes this device from your account.' },
  'devices.send': { vi: 'Gửi', en: 'Send' },
  'devices.cancel': { vi: 'Huỷ', en: 'Cancel' },
  'devices.sending': { vi: 'Đang gửi…', en: 'Sending…' },
  'devices.sentReloading': { vi: 'Đã gửi -- đang tải lại…', en: 'Sent -- reloading…' },
  'devices.failed': { vi: 'Thất bại (HTTP {status}).', en: 'Failed (HTTP {status}).' },

  // Telemetry
  'telemetry.empty': { vi: 'Chưa có dữ liệu cảm biến.', en: 'No sensor data yet.' },
  'telemetry.temperature': { vi: 'Nhiệt độ', en: 'Temperature' },
  'telemetry.humidity': { vi: 'Độ ẩm không khí', en: 'Humidity' },
  'telemetry.soil': { vi: 'Độ ẩm đất', en: 'Soil moisture' },
  'telemetry.water': { vi: 'Mực nước', en: 'Water level' },
  'telemetry.updated': { vi: 'Cập nhật {date}', en: 'Updated {date}' },

  // Conversations
  'discussions.empty': { vi: 'Chưa có hội thoại nào.', en: 'No conversations yet.' },
  'discussions.loadFailed': { vi: 'Không thể tải lịch sử hội thoại.', en: 'Could not load conversation history.' },
  'discussions.play': { vi: '▶ Nghe bản ghi', en: '▶ Play recording' },
  'discussions.loading': { vi: 'Đang tải…', en: 'Loading…' },
  'discussions.playFailed': { vi: 'Không thể tải bản ghi.', en: 'Could not load recording.' },
} as const satisfies Record<string, Record<Lang, string>>

export type StringKey = keyof typeof strings
export type Vars = Record<string, string | number>

export type I18n = {
  lang: Lang
  setLang: (lang: Lang) => void
  t: (key: StringKey, vars?: Vars) => string
  formatDate: (iso: string) => string
}

export const I18nContext = createContext<I18n | null>(null)

export function useI18n(): I18n {
  const ctx = useContext(I18nContext)
  if (!ctx) throw new Error('useI18n must be used inside I18nProvider')
  return ctx
}

