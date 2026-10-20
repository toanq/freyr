import type { SynoraLink } from './link'
import {
  DEFAULT_OTA_CHUNK_BYTES,
  MIN_OTA_CHUNK_BYTES,
  SynoraBleUuids,
  chunkBytes,
  otaStartCommand,
  parseOtaStatus,
} from './protocol'

export class OtaDeviceError extends Error {}

export type OtaPhase = 'uploading' | 'committing'

// BLE firmware push, same protocol as the phone app:
//   control "ota_start:<size>:<sha256>" -> raw chunks on the firmware
//   characteristic -> control "ota_commit" -> device verifies, flashes and
//   reboots (success = the link drops). status notifies
//   "ota_progress:<n>" / "ota_error:<msg>" throughout.
//
// Chunking: Web Bluetooth hides the negotiated MTU, so this starts at
// DEFAULT_OTA_CHUNK_BYTES with write-without-response (fast; Chrome queues
// and back-pressures each awaited write). If the very first write is
// rejected it retries with acknowledged writes, then with 20-byte chunks --
// slower, but works on a link still at the default 23-byte MTU. The device
// enforces size + SHA-256 itself, so a partial/garbled push is rejected at
// commit rather than flashed.
export async function pushFirmware(
  link: SynoraLink,
  bytes: Uint8Array<ArrayBuffer>,
  sha256: string,
  onProgress: (phase: OtaPhase, fraction: number) => void,
  isCancelled: () => boolean,
): Promise<void> {
  let deviceError: string | null = null
  let resolveDone: (error: string | null) => void = () => {}
  const done = new Promise<string | null>((resolve) => (resolveDone = resolve))

  const unsubscribe = await link.subscribe(SynoraBleUuids.status, (text) => {
    const status = parseOtaStatus(text)
    if (status?.kind === 'error') {
      deviceError = status.message
      resolveDone(status.message)
    }
  })
  const stopDisconnect = link.onDisconnect(() => resolveDone(null))

  try {
    await link.writeText(SynoraBleUuids.control, otaStartCommand(bytes.length, sha256))

    const modes: { size: number; withResponse: boolean }[] = [
      { size: DEFAULT_OTA_CHUNK_BYTES, withResponse: false },
      { size: DEFAULT_OTA_CHUNK_BYTES, withResponse: true },
      { size: MIN_OTA_CHUNK_BYTES, withResponse: true },
    ]
    let mode = 0
    let sent = 0
    let lastReport = 0
    for (;;) {
      try {
        for (const chunk of chunkBytes(bytes.subarray(sent) as Uint8Array<ArrayBuffer>, modes[mode].size)) {
          if (isCancelled()) throw new Error('cancelled')
          if (deviceError) throw new OtaDeviceError(deviceError)
          await link.writeBytes(SynoraBleUuids.firmware, chunk, modes[mode].withResponse)
          sent += chunk.length
          const now = performance.now()
          if (now - lastReport > 100 || sent === bytes.length) {
            lastReport = now
            onProgress('uploading', sent / bytes.length)
          }
        }
        break
      } catch (err) {
        // Only the first write decides the mode; later failures are real.
        if (sent > 0 || mode === modes.length - 1 || err instanceof OtaDeviceError || isCancelled()) throw err
        mode++
      }
    }

    if (deviceError) throw new OtaDeviceError(deviceError)
    onProgress('committing', 1)
    try {
      await link.writeText(SynoraBleUuids.control, 'ota_commit')
    } catch (err) {
      // A reboot racing the write acknowledgement still means the commit landed.
      if (link.connected) throw err
    }

    // Commit verifies the whole image on the device (several seconds), then
    // it reboots. Treat "link dropped within 30 s, no ota_error" as success.
    const result = await Promise.race([
      done,
      new Promise<'timeout'>((resolve) => setTimeout(() => resolve('timeout'), 30_000)),
    ])
    if (typeof result === 'string' && result !== 'timeout') throw new OtaDeviceError(result)
  } finally {
    stopDisconnect()
    unsubscribe()
  }
}
