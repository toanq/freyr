import { useEffect, useRef, useState } from 'react'
import { apiFetch } from '../api'
import { useI18n } from '../i18n'
import { SynoraLink, UserCancelledError, chooseSynoraDevice, holdWakeLock, isBluetoothSupported } from './link'
import { OtaDeviceError, pushFirmware, type OtaPhase } from './otaFlow'
import { sha256Hex } from './protocol'
import './ble.css'

type Build = { id: string; version: string; sizeBytes: number; sha256: string }

type State =
  | { kind: 'idle' }
  | { kind: 'busy'; label: string; fraction: number | null }
  | { kind: 'done' }
  | { kind: 'error'; message: string }

// Admin fallback for a device with no Wi-Fi: same push as the phone app's
// BleFirmwareUpdateScreen (download build -> ota_start -> chunks -> ota_commit).
function BleFirmwareUpdate({ builds }: { builds: Build[] }) {
  const { t } = useI18n()
  const [buildId, setBuildId] = useState('')
  const [state, setState] = useState<State>({ kind: 'idle' })
  const linkRef = useRef<SynoraLink | null>(null)
  const cancelled = useRef(false)

  useEffect(() => {
    cancelled.current = false
    return () => {
      cancelled.current = true
      linkRef.current?.close()
    }
  }, [])

  const selected = builds.find((b) => b.id === buildId) ?? builds[0]

  if (!isBluetoothSupported()) {
    return (
      <div className="card ble-ota">
        <h3>{t('bleOta.title')}</h3>
        <p className="ble-unsupported">{t('ble.unsupported')}</p>
      </div>
    )
  }

  const run = async () => {
    if (!selected) return
    let device: BluetoothDevice
    try {
      device = await chooseSynoraDevice()
    } catch (err) {
      if (!(err instanceof UserCancelledError)) setState({ kind: 'error', message: String(err) })
      return
    }

    const release = await holdWakeLock()
    linkRef.current?.close()
    const link = new SynoraLink(device)
    linkRef.current = link
    try {
      setState({ kind: 'busy', label: t('bleOta.downloading'), fraction: null })
      const res = await apiFetch(`/firmware/${selected.id}/binary`)
      if (!res.ok) throw new Error(t('bleOta.errDownload', { status: res.status }))
      const bytes = new Uint8Array(await res.arrayBuffer())
      if (bytes.length !== selected.sizeBytes) {
        throw new Error(t('bleOta.errIncomplete', { got: bytes.length, expected: selected.sizeBytes }))
      }
      if ((await sha256Hex(bytes)) !== selected.sha256.toLowerCase()) throw new Error(t('bleOta.errChecksum'))
      if (cancelled.current) return

      setState({ kind: 'busy', label: t('ble.connecting'), fraction: null })
      await link.connect()

      const labels: Record<OtaPhase, string> = {
        uploading: t('bleOta.uploading'),
        committing: t('bleOta.committing'),
      }
      await pushFirmware(
        link,
        bytes,
        selected.sha256,
        (phase, fraction) => !cancelled.current && setState({ kind: 'busy', label: labels[phase], fraction }),
        () => cancelled.current,
      )
      if (!cancelled.current) setState({ kind: 'done' })
    } catch (err) {
      if (cancelled.current) return
      const message =
        err instanceof OtaDeviceError
          ? t('bleOta.errDevice', { error: err.message })
          : err instanceof Error
            ? err.message
            : String(err)
      setState({ kind: 'error', message })
    } finally {
      release()
      link.close()
      if (linkRef.current === link) linkRef.current = null
    }
  }

  const busy = state.kind === 'busy'

  return (
    <div className="card ble-ota">
      <h3>{t('bleOta.title')}</h3>
      <p className="form-hint">{t('bleOta.hint')}</p>
      {builds.length === 0 ? (
        <p className="form-hint">{t('bleOta.noBuilds')}</p>
      ) : (
        <div className="inline-form">
          <select
            aria-label={t('bleOta.pickBuild')}
            value={selected?.id ?? ''}
            onChange={(e) => setBuildId(e.target.value)}
            disabled={busy}
          >
            {builds.map((b) => (
              <option key={b.id} value={b.id}>
                {b.version} · {(b.sizeBytes / (1024 * 1024)).toFixed(1)} MB
              </option>
            ))}
          </select>
          <button type="button" className="btn-primary" onClick={run} disabled={busy || !selected}>
            {t('bleOta.start')}
          </button>
        </div>
      )}

      {busy && (
        <div className="ble-progress" role="status">
          <p>{state.label}</p>
          <div className="ble-progress-bar">
            <div
              className={state.fraction === null ? 'ble-progress-fill ble-progress-fill--indeterminate' : 'ble-progress-fill'}
              style={state.fraction === null ? undefined : { width: `${Math.round(state.fraction * 100)}%` }}
            />
          </div>
          {state.fraction !== null && <p className="form-hint">{Math.round(state.fraction * 100)}%</p>}
          <p className="form-hint">{t('bleOta.keepOpen')}</p>
        </div>
      )}
      {state.kind === 'done' && <p className="form-status">{t('bleOta.done')}</p>}
      {state.kind === 'error' && <p className="form-error">{state.message}</p>}
    </div>
  )
}

export default BleFirmwareUpdate
