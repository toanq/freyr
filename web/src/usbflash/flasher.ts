/// <reference types="w3c-web-serial" />
import { ESPLoader, Transport, type FlashFreqValues, type FlashModeValues, type FlashSizeValues } from 'esptool-js'
import { apiFetch } from '../api'
import { md5Hex } from './md5'
import {
  PARTITION_TABLE_OFFSET,
  PARTITION_TABLE_SIZE,
  findUpdateLayout,
  overlaps,
  parsePartitionTable,
} from './partitions'

export type KitFile = { name: string; offset: number; size: number; sha256: string; role: string | null }
export type FlashManifest = { chip: string; flashMode: string; flashFreq: string; flashSize: string; files: KitFile[] }
export type FlashImage = { name: string; offset: number; data: Uint8Array }
export type Log = (line: string) => void

export const EXPECTED_CHIP = 'ESP32-S3'
const BAUD_RATES = [921600, 115200]

export function webSerialSupported(): boolean {
  return typeof navigator !== 'undefined' && 'serial' in navigator && window.isSecureContext
}

export async function fetchManifest(buildId: string): Promise<FlashManifest | null> {
  const res = await apiFetch(`/admin/firmware/${buildId}/flash-manifest`)
  if (res.status === 404) return null
  if (!res.ok) throw new Error(`Could not load flash manifest (HTTP ${res.status}).`)
  return res.json()
}

async function sha256Hex(data: Uint8Array): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', data as Uint8Array<ArrayBuffer>)
  return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, '0')).join('')
}

async function download(path: string, expectedSha?: string): Promise<Uint8Array> {
  const res = await apiFetch(path)
  if (!res.ok) throw new Error(`Download failed for ${path} (HTTP ${res.status}).`)
  const data = new Uint8Array(await res.arrayBuffer())
  if (expectedSha && (await sha256Hex(data)) !== expectedSha) {
    throw new Error(`Checksum mismatch for ${path} -- download corrupted, try again.`)
  }
  return data
}

export function downloadKitFile(buildId: string, file: KitFile): Promise<Uint8Array> {
  const name = file.name.split('/').map(encodeURIComponent).join('/')
  return download(`/admin/firmware/${buildId}/flash-files/${name}`, file.sha256)
}

export function downloadAppImage(buildId: string, sha256: string): Promise<Uint8Array> {
  return download(`/firmware/${buildId}/binary`, sha256)
}

export class UsbFlasher {
  private transport: Transport | null = null
  private loader: ESPLoader | null = null
  private log: Log

  constructor(log: Log) {
    this.log = log
  }

  get connected(): boolean {
    return this.loader !== null
  }

  // Must be called from a user gesture (requestPort shows the browser's port picker).
  async connect(): Promise<string> {
    const port = await navigator.serial.requestPort()
    const terminal = {
      clean: () => {},
      writeLine: (line: string) => this.log(line),
      write: (text: string) => text.trim() && this.log(text.trim()),
    }
    let lastError: unknown = null
    for (const baudrate of BAUD_RATES) {
      const transport = new Transport(port, false)
      const loader = new ESPLoader({ transport, baudrate, terminal })
      try {
        this.log(`Connecting at ${baudrate} baud…`)
        const description = await loader.main()
        const chipName = loader.chip.CHIP_NAME
        if (chipName !== EXPECTED_CHIP) {
          await transport.disconnect()
          throw new WrongChipError(`Connected to ${chipName}, but Synora boards are ${EXPECTED_CHIP}. Nothing was written.`)
        }
        this.transport = transport
        this.loader = loader
        return description
      } catch (err) {
        if (err instanceof WrongChipError) throw err
        lastError = err
        await transport.disconnect().catch(() => {})
        this.log(`Failed at ${baudrate} baud: ${errorText(err)}`)
      }
    }
    throw new Error(`Could not connect: ${errorText(lastError)}. Hold BOOT while plugging in, then retry.`)
  }

  // Keep-settings update: app into ota_0 plus otadata reset so the bootloader
  // picks ota_0. Offsets come from the device's own partition table, so NVS
  // (Wi-Fi credentials, pairing) is never touched.
  async planUpdate(app: Uint8Array, otadataImage: Uint8Array | null, kit: FlashManifest | null): Promise<FlashImage[]> {
    const loader = this.requireLoader()
    this.log('Reading partition table from the device…')
    const table = await loader.readFlash(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE)
    const layout = findUpdateLayout(parsePartitionTable(table))
    if (!layout) {
      throw new Error('Device has no ota_0/otadata partitions (blank or foreign firmware). Use Full flash instead.')
    }
    const kitApp = kit?.files.find((f) => f.role === 'app')
    if (kitApp && kitApp.offset !== layout.ota0.offset) {
      throw new Error(
        `This build expects ota_0 at 0x${kitApp.offset.toString(16)}, the device has it at 0x${layout.ota0.offset.toString(16)}. ` +
          'The partition layout changed -- use Full flash.',
      )
    }
    if (app.length > layout.ota0.size) throw new Error('App image is larger than the device ota_0 partition.')
    const otadata =
      otadataImage && otadataImage.length === layout.otadata.size
        ? otadataImage
        : new Uint8Array(layout.otadata.size).fill(0xff)
    const images = [
      { name: 'otadata', offset: layout.otadata.offset, data: otadata },
      { name: 'app (ota_0)', offset: layout.ota0.offset, data: app },
    ]
    if (layout.nvs && images.some((img) => overlaps(img.offset, img.data.length, layout.nvs!))) {
      throw new Error('Refusing to flash: write would overlap the NVS partition.')
    }
    return images
  }

  async flash(
    images: FlashImage[],
    settings: { flashMode: string; flashFreq: string; flashSize: string; eraseAll: boolean },
    onProgress: (fileIndex: number, written: number, total: number) => void,
  ): Promise<void> {
    const loader = this.requireLoader()
    if (settings.eraseAll) {
      this.log('Erasing entire flash (this takes a while)…')
      await loader.eraseFlash()
    }
    for (const img of images) this.log(`Writing ${img.name} (${img.data.length} bytes) at 0x${img.offset.toString(16)}`)
    await loader.writeFlash({
      fileArray: images.map((img) => ({ data: img.data, address: img.offset })),
      flashMode: settings.flashMode as FlashModeValues,
      flashFreq: settings.flashFreq as FlashFreqValues,
      flashSize: settings.flashSize as FlashSizeValues,
      eraseAll: false,
      compress: true,
      reportProgress: onProgress,
      calculateMD5Hash: md5Hex,
    })
    this.log('Flash verified (MD5). Resetting the board…')
    await loader.after('hard_reset')
  }

  async disconnect(): Promise<void> {
    const transport = this.transport
    this.transport = null
    this.loader = null
    if (transport) await transport.disconnect().catch(() => {})
  }

  private requireLoader(): ESPLoader {
    if (!this.loader) throw new Error('Not connected.')
    return this.loader
  }
}

class WrongChipError extends Error {}

export function errorText(err: unknown): string {
  if (err instanceof Error) return err.message
  return String(err)
}
