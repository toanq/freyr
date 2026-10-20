import type { SynoraLink } from './link'
import {
  SynoraBleUuids,
  macFromDeviceId,
  mergeWifiNetworks,
  parseDeviceInfo,
  parseWifiScanChunk,
  type DeviceInfo,
  type WifiNetwork,
} from './protocol'

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))

// Subscribes to `uuid` (awaited, so no notify can slip past), runs `trigger`,
// then resolves with the first notified text, or null after `ms`.
async function notifyAfter(link: SynoraLink, uuid: string, ms: number, trigger: () => Promise<void>) {
  let resolveNext: (text: string | null) => void = () => {}
  const next = new Promise<string | null>((resolve) => (resolveNext = resolve))
  const unsubscribe = await link.subscribe(uuid, (text) => resolveNext(text))
  const timer = setTimeout(() => resolveNext(null), ms)
  try {
    await trigger()
    return await next
  } finally {
    clearTimeout(timer)
    unsubscribe()
  }
}

export async function readDeviceInfo(link: SynoraLink): Promise<DeviceInfo | null> {
  try {
    return parseDeviceInfo(await link.readText(SynoraBleUuids.deviceInfo))
  } catch {
    return null
  }
}

// One chunk round trip: write the command, wait for the device's "chunk
// ready" notify (its payload is ignored -- a notify is capped at MTU-3 bytes),
// then read the full chunk. A real scan can take up to ~40 s on the device.
async function scanChunk(link: SynoraLink, command: 'rescan' | 'more_aps') {
  await notifyAfter(link, SynoraBleUuids.wifiScan, 45_000, () => link.writeText(SynoraBleUuids.control, command))
  return parseWifiScanChunk(await link.readText(SynoraBleUuids.wifiScan))
}

// Fresh scan on the device, paginated ("rescan" then "more_aps" while more
// remain). Up to 4 attempts: a scan during an active BLE connection can come
// back empty because Wi-Fi/BLE share the radio.
export async function scanWifiNetworks(link: SynoraLink): Promise<WifiNetwork[]> {
  let networks: WifiNetwork[] = []
  for (let attempt = 0; attempt < 4 && networks.length === 0; attempt++) {
    if (attempt > 0) await sleep(700)
    try {
      const all: WifiNetwork[] = []
      let command: 'rescan' | 'more_aps' = 'rescan'
      for (let page = 0; page < 20; page++) {
        const chunk = await scanChunk(link, command)
        all.push(...chunk.networks)
        if (!chunk.hasMore) break
        command = 'more_aps'
      }
      networks = mergeWifiNetworks(all)
    } catch {
      // retried
    }
  }
  return networks
}

export type ProvisionStage = 'joining' | 'activating'
export type ProvisionFailure = 'wifiTimeout' | 'wifiError' | 'activationTimeout' | 'activationError' | 'noCode'
export class ProvisionError extends Error {
  readonly reason: ProvisionFailure
  constructor(reason: ProvisionFailure) {
    super(reason)
    this.reason = reason
  }
}

const STAGE_TIMEOUT_MS = 120_000

// Writes server URL + Wi-Fi credentials, commits, then follows the status
// characteristic through online -> activated. The device drops BLE while it
// joins Wi-Fi; this reconnects and resubscribes so the notifications keep
// flowing. Returns the activation code and (best-effort) MAC address.
export async function provisionDevice(
  link: SynoraLink,
  opts: { serverUrl: string; ssid: string; password: string },
  onStage: (stage: ProvisionStage) => void,
  isCancelled: () => boolean,
): Promise<{ code: string; macAddress?: string }> {
  let onlineResolve: ((s: string) => void) | null = null
  let activatedResolve: ((s: string) => void) | null = null
  const online = new Promise<string>((r) => (onlineResolve = r))
  const activated = new Promise<string>((r) => (activatedResolve = r))
  let onlineDone = false
  let activatedDone = false

  const handleStatus = (s: string) => {
    // "activated" implies online already happened even if that notify was lost mid-reconnect.
    if (!onlineDone && (s === 'online' || s === 'activated' || s === 'error')) {
      onlineDone = true
      onlineResolve!(s === 'activated' ? 'online' : s)
    }
    if (!activatedDone && (s === 'activated' || s === 'error')) {
      activatedDone = true
      activatedResolve!(s)
    }
  }

  let unsubscribeStatus = await link.subscribe(SynoraBleUuids.status, handleStatus)
  const stopReconnect = link.onDisconnect(() => {
    if ((onlineDone && activatedDone) || isCancelled()) return
    void link.reconnect().then(async (ok) => {
      if (!ok || isCancelled()) return
      try {
        unsubscribeStatus = await link.subscribe(SynoraBleUuids.status, handleStatus)
      } catch {
        // the stage timeouts below are the safety net
      }
    })
  })

  const timeout = (p: Promise<string>) =>
    Promise.race([p, sleep(STAGE_TIMEOUT_MS).then(() => 'timeout')])

  try {
    await link.writeText(SynoraBleUuids.serverUrl, opts.serverUrl)
    await link.writeText(SynoraBleUuids.wifiSsid, opts.ssid)
    await link.writeText(SynoraBleUuids.wifiPassword, opts.password)
    await link.writeText(SynoraBleUuids.control, 'commit')

    onStage('joining')
    const wifi = await timeout(online)
    if (isCancelled()) throw new ProvisionError('wifiTimeout')
    if (wifi !== 'online') throw new ProvisionError(wifi === 'timeout' ? 'wifiTimeout' : 'wifiError')

    onStage('activating')
    const final = await timeout(activated)
    if (isCancelled()) throw new ProvisionError('activationTimeout')
    if (final !== 'activated') throw new ProvisionError(final === 'timeout' ? 'activationTimeout' : 'activationError')
  } finally {
    stopReconnect()
    unsubscribeStatus()
  }

  if (!link.connected) await link.reconnect()
  const code = (await link.readText(SynoraBleUuids.activation)).trim()
  if (!code) throw new ProvisionError('noCode')
  const info = await readDeviceInfo(link)
  return { code, macAddress: macFromDeviceId(info?.deviceId) }
}

// The device's cached code points at a server row that no longer exists
// (404 on pair) -- ask it to mint a fresh code and wait for the notify.
export async function requestFreshActivationCode(link: SynoraLink): Promise<string | null> {
  const code = await notifyAfter(link, SynoraBleUuids.activation, 15_000, () =>
    link.writeText(SynoraBleUuids.control, 'reactivate'),
  )
  return code?.trim() || null
}
