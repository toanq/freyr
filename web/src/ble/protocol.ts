// Pure (DOM-free) pieces of the Synora BLE provisioning protocol, kept
// separate from the GATT I/O so they can be unit-tested in plain Node.
// Authoritative contract: firmware-synora/main/boards/common/ble_provisioning.cc
// (mirrored by the phone app's app/lib/devices/ble_pairing_screen.dart).

// Must match the firmware's 128-bit UUIDs exactly (lowercase, as Web Bluetooth requires).
export const SynoraBleUuids = {
  service: 'd2270ba0-a51c-4d67-8253-08367721e4e7',
  wifiSsid: 'f0cc2af8-8477-443a-a981-aa1ea57655c6',
  wifiPassword: '518c94dd-3b10-4ccc-96a4-30bb859d2620',
  wifiScan: 'b5233f6f-0b5d-4ac0-983c-c5c7978f4476',
  serverUrl: '1413dc99-e1fd-4991-9c88-6d8e535dbf1a',
  deviceInfo: 'ca17f48a-2be1-456b-934e-96440927b009',
  settings: 'eeb5de90-1c07-402a-933c-bc0c3149a3d3',
  activation: 'b2af9052-c495-4064-a949-a4ff16f17572',
  control: '0d128bcf-fd9d-4288-97fb-b11b430e45aa',
  status: '483d3f05-a014-4990-a511-fc9863763cca',
  firmware: '917c4e8b-025d-3a9f-1e4c-776a2d1b8e4f',
} as const

export type WifiNetwork = { ssid: string; rssi: number }

const encoder = new TextEncoder()
const decoder = new TextDecoder()

export function encodeText(text: string): Uint8Array<ArrayBuffer> {
  return encoder.encode(text) as Uint8Array<ArrayBuffer>
}

export function decodeText(view: DataView | ArrayBuffer | Uint8Array): string {
  if (view instanceof DataView) return decoder.decode(new Uint8Array(view.buffer, view.byteOffset, view.byteLength))
  return decoder.decode(view)
}

// One wifi_scan chunk: {"more":bool,"items":[[ssid,rssi],...]} -- compact
// [ssid, rssi] pairs keep each chunk under BLE's 512-byte attribute ceiling.
export function parseWifiScanChunk(json: string): { networks: WifiNetwork[]; hasMore: boolean } {
  const decoded = JSON.parse(json) as { more?: unknown; items?: unknown }
  if (!Array.isArray(decoded.items)) throw new Error('wifi_scan: missing items')
  const networks = decoded.items
    .filter((e): e is [string, number] => Array.isArray(e) && typeof e[0] === 'string' && typeof e[1] === 'number')
    .map(([ssid, rssi]) => ({ ssid, rssi }))
  return { networks, hasMore: decoded.more === true }
}

// Drops empty (hidden) SSIDs, keeps the strongest entry per SSID, strongest first.
export function mergeWifiNetworks(networks: WifiNetwork[]): WifiNetwork[] {
  const best = new Map<string, WifiNetwork>()
  for (const n of networks) {
    if (!n.ssid) continue
    const prev = best.get(n.ssid)
    if (!prev || n.rssi > prev.rssi) best.set(n.ssid, n)
  }
  return [...best.values()].sort((a, b) => b.rssi - a.rssi)
}

export type DeviceInfo = { deviceId?: string; firmwareVersion?: string; isPaired?: boolean }

export function parseDeviceInfo(json: string): DeviceInfo {
  const raw = JSON.parse(json) as Record<string, unknown>
  return {
    deviceId: typeof raw.deviceId === 'string' ? raw.deviceId : undefined,
    firmwareVersion: typeof raw.firmwareVersion === 'string' ? raw.firmwareVersion : undefined,
    isPaired: raw.isPaired === true,
  }
}

// deviceInfo.deviceId is the MAC as 12 hex chars without separators.
export function macFromDeviceId(deviceId: string | undefined): string | undefined {
  if (!deviceId || !/^[0-9a-fA-F]{12}$/.test(deviceId)) return undefined
  return deviceId.match(/.{2}/g)!.join(':').toUpperCase()
}

// Provisioning states the status characteristic reports outside an OTA.
export type ProvisioningStatus = 'idle' | 'joining' | 'online' | 'activated' | 'error'

export type OtaStatus = { kind: 'progress'; percent: number } | { kind: 'error'; message: string }

// status during a firmware push: "ota_progress:<n>" / "ota_error:<msg>".
export function parseOtaStatus(text: string): OtaStatus | null {
  if (text.startsWith('ota_error:')) return { kind: 'error', message: text.slice('ota_error:'.length) }
  if (text.startsWith('ota_progress:')) {
    const percent = Number.parseInt(text.slice('ota_progress:'.length), 10)
    if (Number.isFinite(percent)) return { kind: 'progress', percent: Math.max(0, Math.min(100, percent)) }
  }
  return null
}

export function otaStartCommand(sizeBytes: number, sha256Hex: string): string {
  if (!Number.isInteger(sizeBytes) || sizeBytes <= 0) throw new Error('ota_start: invalid size')
  if (!/^[0-9a-fA-F]{64}$/.test(sha256Hex)) throw new Error('ota_start: invalid sha256')
  return `ota_start:${sizeBytes}:${sha256Hex.toLowerCase()}`
}

// Web Bluetooth never exposes the negotiated ATT MTU. 180 bytes fits an MTU
// of 185, the common floor once a phone/PC negotiates above the 23-byte
// default (Chrome on Android asks for 517; the firmware prefers 517) -- big
// enough to keep a multi-MB push practical, small enough not to be rejected.
export const DEFAULT_OTA_CHUNK_BYTES = 180
// Last-resort size for links stuck at the default 23-byte MTU (23 - 3 header).
export const MIN_OTA_CHUNK_BYTES = 20

export function* chunkBytes(bytes: Uint8Array, chunkSize: number): Generator<Uint8Array<ArrayBuffer>> {
  if (chunkSize <= 0) throw new Error('chunkSize must be > 0')
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    yield bytes.slice(offset, offset + chunkSize) as Uint8Array<ArrayBuffer>
  }
}

// The device needs an absolute URL for the API it activates against; the web
// console's API_URL is usually relative ("/api") behind Traefik.
export function absoluteServerUrl(apiUrl: string, origin: string): string {
  return new URL(apiUrl, origin).href.replace(/\/+$/, '')
}

export async function sha256Hex(bytes: Uint8Array<ArrayBuffer>): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', bytes)
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('')
}
