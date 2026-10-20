import { lazy, Suspense, useEffect, useRef, useState } from 'react'
import { apiFetch, API_URL } from './api'
import './Firmware.css'

// esptool-js is ~175 kB; only admins opening this tab should download it.
const UsbFlash = lazy(() => import('./UsbFlash'))

type Build = {
  id: string
  version: string
  sizeBytes: number
  sha256: string
  createdAt: string
  notes: string | null
  hasFlashKit: boolean
}

type Device = {
  id: string
  name: string
  owner: string | null
  isOnline: boolean
  firmwareVersion: string | null
}

type Status = { kind: 'ok' | 'error' | 'info'; text: string } | null

function Firmware() {
  const [builds, setBuilds] = useState<Build[] | null>(null)
  const [devices, setDevices] = useState<Device[] | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)

  const loadBuilds = () =>
    apiFetch('/admin/firmware')
      .then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        return res.json()
      })
      .then(setBuilds)
      .catch((err) => setLoadError(`Could not load firmware builds (${err}).`))

  const loadDevices = () =>
    apiFetch('/devices')
      .then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        return res.json()
      })
      .then(setDevices)
      .catch((err) => setLoadError(`Could not load devices (${err}).`))

  useEffect(() => {
    loadBuilds()
    loadDevices()
  }, [])

  return (
    <section className="firmware">
      <h2>Firmware</h2>
      {loadError && <p className="form-error">{loadError}</p>}
      <UploadCard onUploaded={loadBuilds} />
      <BuildList builds={builds} onDeleted={loadBuilds} />
      <Suspense fallback={<div className="card fw-usb"><p className="form-hint">Loading USB flasher…</p></div>}>
        <UsbFlash builds={builds ?? []} />
      </Suspense>
      <DevicePanel builds={builds ?? []} devices={devices} onRefresh={loadDevices} />
    </section>
  )
}

function UploadCard({ onUploaded }: { onUploaded: () => void }) {
  const [file, setFile] = useState<File | null>(null)
  const [version, setVersion] = useState('')
  const [notes, setNotes] = useState('')
  const [progress, setProgress] = useState<number | null>(null)
  const [status, setStatus] = useState<Status>(null)
  const fileInput = useRef<HTMLInputElement>(null)

  const uploadErrorText = (code: number, body: string) => {
    if (code === 409) return `Version "${version.trim()}" already exists -- bump the version and try again.`
    if (code === 413) return 'File is too large (limit 8 MB for a .bin, 32 MB for a flash kit).'
    if (code === 400) return `Rejected: ${body || 'not a valid ESP32 firmware image or flash kit.'}`
    if (code === 403) return 'Only admins can upload firmware.'
    return `Upload failed (HTTP ${code}).`
  }

  // XHR rather than fetch: fetch can't report upload progress.
  const upload = (e: React.FormEvent) => {
    e.preventDefault()
    if (!file || !version.trim()) return
    const form = new FormData()
    form.append('file', file)
    form.append('version', version.trim())
    if (notes.trim()) form.append('notes', notes.trim())

    const xhr = new XMLHttpRequest()
    xhr.open('POST', `${API_URL}/admin/firmware`)
    xhr.withCredentials = true
    xhr.upload.onprogress = (ev) => ev.lengthComputable && setProgress(Math.round((ev.loaded / ev.total) * 100))
    xhr.onload = () => {
      setProgress(null)
      if (xhr.status >= 200 && xhr.status < 300) {
        setStatus({ kind: 'ok', text: `Uploaded ${version.trim()}.` })
        setFile(null)
        setVersion('')
        setNotes('')
        if (fileInput.current) fileInput.current.value = ''
        onUploaded()
      } else {
        setStatus({ kind: 'error', text: uploadErrorText(xhr.status, xhr.responseText.slice(0, 300)) })
      }
    }
    xhr.onerror = () => {
      setProgress(null)
      setStatus({ kind: 'error', text: 'Network error during upload.' })
    }
    setStatus(null)
    setProgress(0)
    xhr.send(form)
  }

  const uploading = progress !== null

  return (
    <form className="card fw-upload" onSubmit={upload}>
      <h3>Upload build</h3>
      <p className="form-hint">
        A <code>.bin</code> app image enables OTA updates. A flash-kit <code>.zip</code> from{' '}
        <code>scripts/package-firmware.sh</code> also enables full USB flashing of new boards.
      </p>
      <input
        ref={fileInput}
        type="file"
        accept=".bin,.zip,application/octet-stream,application/zip"
        onChange={(e) => setFile(e.target.files?.[0] ?? null)}
        disabled={uploading}
        required
      />
      <div className="inline-form">
        <input
          placeholder="Version, e.g. 1.2.3"
          value={version}
          onChange={(e) => setVersion(e.target.value)}
          disabled={uploading}
          required
        />
        <input placeholder="Notes (optional)" value={notes} onChange={(e) => setNotes(e.target.value)} disabled={uploading} />
        <button type="submit" disabled={uploading || !file || !version.trim()}>
          {uploading ? `Uploading ${progress}%` : 'Upload'}
        </button>
      </div>
      {uploading && (
        <div className="fw-progress" role="progressbar" aria-valuenow={progress ?? 0} aria-valuemin={0} aria-valuemax={100}>
          <div style={{ width: `${progress}%` }} />
        </div>
      )}
      {status && <p className={status.kind === 'ok' ? 'form-status' : 'form-error'}>{status.text}</p>}
    </form>
  )
}

function BuildList({ builds, onDeleted }: { builds: Build[] | null; onDeleted: () => void }) {
  const [status, setStatus] = useState<Status>(null)
  const [copiedId, setCopiedId] = useState<string | null>(null)

  const remove = async (b: Build) => {
    if (!window.confirm(`Delete firmware ${b.version}? Devices can no longer be updated to it.`)) return
    const res = await apiFetch(`/admin/firmware/${b.id}`, { method: 'DELETE' })
    if (res.ok) {
      setStatus({ kind: 'ok', text: `Deleted ${b.version}.` })
      onDeleted()
    } else {
      setStatus({ kind: 'error', text: res.status === 404 ? `${b.version} was already deleted.` : `Delete failed (HTTP ${res.status}).` })
      if (res.status === 404) onDeleted()
    }
  }

  const copy = async (b: Build) => {
    try {
      await navigator.clipboard.writeText(b.sha256)
      setCopiedId(b.id)
      setTimeout(() => setCopiedId((id) => (id === b.id ? null : id)), 1500)
    } catch {
      setStatus({ kind: 'info', text: `SHA-256 of ${b.version}: ${b.sha256}` })
    }
  }

  return (
    <div className="card fw-builds">
      <div className="page-header">
        <h3>Builds</h3>
        {builds && <span className="page-count">{builds.length}</span>}
      </div>
      {status && <p className={status.kind === 'error' ? 'form-error' : 'form-status'}>{status.text}</p>}
      {builds === null && <p className="form-hint">Loading…</p>}
      {builds?.length === 0 && <p className="form-hint">No builds uploaded yet.</p>}
      <ul className="fw-build-list">
        {builds?.map((b, i) => (
          <li key={b.id} className="fw-build">
            <div className="fw-build-main">
              <span className="fw-version">{b.version}</span>
              {i === 0 && <span className="chip fw-latest">latest</span>}
              {b.hasFlashKit && (
                <span className="chip fw-kit" title="Uploaded as a flash kit -- supports full USB flashing">
                  USB kit
                </span>
              )}
              <button type="button" className="btn-danger fw-delete" onClick={() => remove(b)}>
                Delete
              </button>
            </div>
            <div className="device-meta">
              <span>{formatSize(b.sizeBytes)}</span>
              <span>{new Date(b.createdAt).toLocaleString()}</span>
              <button type="button" className="fw-sha" title={`${b.sha256} (click to copy)`} onClick={() => copy(b)}>
                {copiedId === b.id ? 'copied' : `sha ${b.sha256.slice(0, 12)}…`}
              </button>
            </div>
            {b.notes && <p className="fw-notes">{b.notes}</p>}
          </li>
        ))}
      </ul>
    </div>
  )
}

type OtaResult = { kind: 'ok' | 'error'; text: string }

async function triggerOta(deviceId: string, buildId: string): Promise<OtaResult> {
  try {
    const res = await apiFetch(`/devices/${deviceId}/ota`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ firmwareBuildId: buildId }),
    })
    if (res.ok) return { kind: 'ok', text: 'Update sent -- device will download and reboot.' }
    if (res.status === 503) return { kind: 'error', text: 'MQTT broker offline -- command not sent.' }
    if (res.status === 400) return { kind: 'error', text: 'Server rejected the build (deleted?).' }
    if (res.status === 404) return { kind: 'error', text: 'Device not found.' }
    return { kind: 'error', text: `Failed (HTTP ${res.status}).` }
  } catch (err) {
    return { kind: 'error', text: `Network error (${err}).` }
  }
}

function DevicePanel({ builds, devices, onRefresh }: { builds: Build[]; devices: Device[] | null; onRefresh: () => void }) {
  const latest = builds[0]
  const [selected, setSelected] = useState<Record<string, string>>({})
  const [results, setResults] = useState<Record<string, OtaResult>>({})
  const [busy, setBusy] = useState<Record<string, boolean>>({})
  const [bulkSummary, setBulkSummary] = useState<string | null>(null)

  const buildFor = (d: Device) => selected[d.id] ?? latest?.id ?? ''
  const outdatedOnline = (devices ?? []).filter((d) => d.isOnline && latest && d.firmwareVersion !== latest.version)

  const update = async (d: Device, buildId: string) => {
    setBusy((b) => ({ ...b, [d.id]: true }))
    const result = await triggerOta(d.id, buildId)
    setResults((r) => ({ ...r, [d.id]: result }))
    setBusy((b) => ({ ...b, [d.id]: false }))
    return result
  }

  const updateAll = async () => {
    if (!latest) return
    if (!window.confirm(`Send firmware ${latest.version} to ${outdatedOnline.length} online device(s)?`)) return
    setBulkSummary('Sending…')
    let ok = 0
    for (const d of outdatedOnline) {
      if ((await update(d, latest.id)).kind === 'ok') ok++
    }
    setBulkSummary(`Sent to ${ok} of ${outdatedOnline.length} device(s).`)
  }

  return (
    <div className="card fw-devices">
      <div className="page-header">
        <h3>Devices</h3>
        <button type="button" className="fw-refresh" onClick={onRefresh}>
          Refresh
        </button>
      </div>
      {latest && outdatedOnline.length > 0 && (
        <div className="fw-bulk">
          <button type="button" className="btn-primary" onClick={updateAll}>
            Update {outdatedOnline.length} outdated online device{outdatedOnline.length === 1 ? '' : 's'} to {latest.version}
          </button>
          {bulkSummary && <span className="form-hint">{bulkSummary}</span>}
        </div>
      )}
      {devices === null && <p className="form-hint">Loading…</p>}
      {devices?.length === 0 && <p className="form-hint">No devices.</p>}
      {builds.length === 0 && devices && devices.length > 0 && <p className="form-hint">Upload a build to update devices.</p>}
      <ul className="fw-device-list">
        {devices?.map((d) => {
          const upToDate = !!latest && d.firmwareVersion === latest.version
          const result = results[d.id]
          return (
            <li key={d.id} className="fw-device">
              <div className="fw-device-head">
                <span className={`fw-dot ${d.isOnline ? 'online' : ''}`} aria-label={d.isOnline ? 'Online' : 'Offline'} />
                <span className="fw-device-name">{d.name}</span>
                {d.owner && <span className="chip">{d.owner}</span>}
              </div>
              <div className="device-meta">
                <span>
                  Running <strong className="fw-running">{d.firmwareVersion ?? 'unknown'}</strong>
                </span>
                {latest && (upToDate ? <span className="device-status online">up to date</span> : <span className="device-status muted">update available</span>)}
              </div>
              {builds.length > 0 && (
                <div className="inline-form fw-device-actions">
                  <select
                    aria-label={`Firmware build for ${d.name}`}
                    value={buildFor(d)}
                    onChange={(e) => setSelected((s) => ({ ...s, [d.id]: e.target.value }))}
                  >
                    {builds.map((b, i) => (
                      <option key={b.id} value={b.id}>
                        {b.version}
                        {i === 0 ? ' (latest)' : ''}
                      </option>
                    ))}
                  </select>
                  <button
                    type="button"
                    disabled={!d.isOnline || busy[d.id]}
                    title={d.isOnline ? undefined : 'Device is offline -- it only receives OTA commands while connected.'}
                    onClick={() => update(d, buildFor(d))}
                  >
                    {busy[d.id] ? 'Sending…' : 'Update'}
                  </button>
                </div>
              )}
              {result && <p className={result.kind === 'ok' ? 'form-status' : 'form-error'}>{result.text}</p>}
            </li>
          )
        })}
      </ul>
    </div>
  )
}

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`
}

export default Firmware
