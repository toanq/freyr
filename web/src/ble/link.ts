import { SynoraBleUuids, decodeText, encodeText } from './protocol'

export function isBluetoothSupported(): boolean {
  return typeof navigator !== 'undefined' && !!navigator.bluetooth && window.isSecureContext
}

export class UserCancelledError extends Error {}

// Opens the browser's device chooser. The service filter matches when the
// device advertises the service UUID; the name prefix is the fallback for
// advertisements that only carry the "Synora-XXXX" name.
export async function chooseSynoraDevice(): Promise<BluetoothDevice> {
  if (!navigator.bluetooth) throw new Error('Web Bluetooth unavailable')
  try {
    return await navigator.bluetooth.requestDevice({
      filters: [{ services: [SynoraBleUuids.service] }, { namePrefix: 'Synora' }],
      optionalServices: [SynoraBleUuids.service],
    })
  } catch (err) {
    if (err instanceof DOMException && err.name === 'NotFoundError') throw new UserCancelledError()
    throw err
  }
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))

// One GATT connection to a Synora device. Characteristic handles are only
// valid for the connection that produced them, so they are cached per
// connection and dropped whenever the link goes down.
export class SynoraLink {
  readonly device: BluetoothDevice
  private service: BluetoothRemoteGATTService | null = null
  private chars = new Map<string, BluetoothRemoteGATTCharacteristic>()
  private closed = false

  constructor(device: BluetoothDevice) {
    this.device = device
    device.addEventListener('gattserverdisconnected', this.handleDisconnect)
  }

  get name(): string {
    return this.device.name ?? this.device.id
  }

  get connected(): boolean {
    return !!this.device.gatt?.connected
  }

  private handleDisconnect = () => {
    this.service = null
    this.chars.clear()
  }

  async connect(): Promise<void> {
    if (!this.device.gatt) throw new Error('Device has no GATT server')
    const server = await this.device.gatt.connect()
    this.service = await server.getPrimaryService(SynoraBleUuids.service)
    this.chars.clear()
  }

  // The device deliberately drops BLE while it joins Wi-Fi and re-advertises
  // right after; retry a few times instead of failing on the first attempt.
  async reconnect(attempts = 6, delayMs = 1500): Promise<boolean> {
    for (let i = 0; i < attempts && !this.closed; i++) {
      try {
        await this.connect()
        return true
      } catch {
        await sleep(delayMs)
      }
    }
    return false
  }

  private async char(uuid: string): Promise<BluetoothRemoteGATTCharacteristic> {
    const cached = this.chars.get(uuid)
    if (cached) return cached
    if (!this.service) await this.connect()
    const c = await this.service!.getCharacteristic(uuid)
    this.chars.set(uuid, c)
    return c
  }

  async readText(uuid: string): Promise<string> {
    return decodeText(await (await this.char(uuid)).readValue())
  }

  async writeText(uuid: string, text: string): Promise<void> {
    await (await this.char(uuid)).writeValueWithResponse(encodeText(text))
  }

  async writeBytes(uuid: string, bytes: Uint8Array<ArrayBuffer>, withResponse: boolean): Promise<void> {
    const c = await this.char(uuid)
    if (withResponse) await c.writeValueWithResponse(bytes)
    else await c.writeValueWithoutResponse(bytes)
  }

  // Raw per-notification text (no replay of a previously cached value).
  async subscribe(uuid: string, onText: (text: string) => void): Promise<() => void> {
    const c = await this.char(uuid)
    const listener = (event: Event) => {
      const value = (event.target as BluetoothRemoteGATTCharacteristic).value
      if (value) onText(decodeText(value))
    }
    c.addEventListener('characteristicvaluechanged', listener)
    await c.startNotifications()
    return () => {
      c.removeEventListener('characteristicvaluechanged', listener)
      c.stopNotifications().catch(() => {})
    }
  }

  onDisconnect(callback: () => void): () => void {
    this.device.addEventListener('gattserverdisconnected', callback)
    return () => this.device.removeEventListener('gattserverdisconnected', callback)
  }

  close(): void {
    this.closed = true
    this.device.removeEventListener('gattserverdisconnected', this.handleDisconnect)
    try {
      this.device.gatt?.disconnect()
    } catch {
      // already gone
    }
  }
}

// Best-effort: pairing has multi-minute waits and a locked screen can
// interrupt BLE I/O on phones (same reason the app holds a wakelock).
export async function holdWakeLock(): Promise<() => void> {
  const wl = (navigator as Navigator & { wakeLock?: { request(type: 'screen'): Promise<{ release(): Promise<void> }> } })
    .wakeLock
  if (!wl) return () => {}
  try {
    const sentinel = await wl.request('screen')
    return () => {
      sentinel.release().catch(() => {})
    }
  } catch {
    return () => {}
  }
}
