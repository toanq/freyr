import { useEffect, useState } from 'react'
import { apiFetch } from './api'

type MicTestUpload = {
  id: string
  deviceId: string
  deviceName: string
  createdAt: string
  sizeBytes: number
}

// Admin-only view of WAV recordings captured by the firmware's diagnostic
// `mictest` console command (synora/devices/{deviceId}/mictest/upload, see
// server's MqttService/MicTestUpload) -- a fleet-wide hardware diagnostic
// tool for whoever administers the devices, not something a regular owner
// needs, so this is gated the same way as DuplicateDevices/AdminTest.
function MicTestUploads() {
  const [uploads, setUploads] = useState<MicTestUpload[] | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)

  useEffect(() => {
    apiFetch('/admin/mictest-uploads')
      .then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        return res.json()
      })
      .then(setUploads)
      .catch((err) => setLoadError(`Could not load mictest uploads (${err}).`))
  }, [])

  return (
    <section className="mictest-uploads">
      <h2>Mic test uploads</h2>
      <p className="admin-test-hint">
        WAV recordings published by the firmware's diagnostic <code>mictest</code> console command over MQTT --
        newest 100 uploads across all devices.
      </p>

      {loadError && <p className="form-error">{loadError}</p>}
      {uploads === null && !loadError && <p>Loading…</p>}
      {uploads !== null && uploads.length === 0 && <p>No mic test uploads yet.</p>}

      {uploads !== null && uploads.length > 0 && (
        <div className="table-scroll card">
        <table>
          <thead>
            <tr>
              <th>Device</th>
              <th>Recorded</th>
              <th>Size</th>
              <th>Recording</th>
            </tr>
          </thead>
          <tbody>
            {uploads.map((u) => (
              <tr key={u.id}>
                <td>{u.deviceName}</td>
                <td>{new Date(u.createdAt).toLocaleString()}</td>
                <td>{formatSize(u.sizeBytes)}</td>
                <td>
                  <MicTestPlayer uploadId={u.id} />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        </div>
      )}
    </section>
  )
}

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  return `${(bytes / 1024).toFixed(1)} KB`
}

// Same lazy-fetch-then-play pattern as Devices.tsx's RecordingPlayer -- only
// fetch the WAV once an admin actually asks to hear it, rather than eagerly
// loading up to 100 recordings on every page load.
function MicTestPlayer({ uploadId }: { uploadId: string }) {
  const [audioUrl, setAudioUrl] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [playerError, setPlayerError] = useState<string | null>(null)

  useEffect(() => {
    return () => {
      if (audioUrl) URL.revokeObjectURL(audioUrl)
    }
  }, [audioUrl])

  const handleLoad = async () => {
    setLoading(true)
    setPlayerError(null)
    try {
      const res = await apiFetch(`/admin/mictest-uploads/${uploadId}/audio`)
      if (!res.ok) throw new Error()
      const blob = await res.blob()
      setAudioUrl(URL.createObjectURL(blob))
    } catch {
      setPlayerError('Could not load recording.')
    } finally {
      setLoading(false)
    }
  }

  if (audioUrl) {
    return <audio className="discussion-recording" controls autoPlay src={audioUrl} />
  }

  return (
    <span className="discussion-recording-controls">
      <button type="button" className="discussion-recording-play" onClick={handleLoad} disabled={loading}>
        {loading ? 'Loading…' : '▶ Play recording'}
      </button>
      {playerError && <span className="form-error">{playerError}</span>}
    </span>
  )
}

export default MicTestUploads
