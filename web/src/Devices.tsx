import { useEffect, useState } from 'react'
import { apiFetch } from './api'
import { useI18n } from './i18n'

type Reading = {
  temperatureCelsius: number | null
  humidityPercent: number | null
  soilMoisturePercent: number | null
  waterLevelPercent: number | null
  recordedAt: string
}

type DeviceRow = {
  id: string
  name: string
  macAddress: string | null
  pairedAt: string | null
  owner: string | null
  latestReading: Reading | null
  isOnline: boolean
  xiaozhiActivationCode: string | null
  muted: boolean
}

export type Discussion = {
  id: string
  sessionId: string
  userText: string
  assistantText: string
  createdAt: string
  hasUserAudio: boolean
}

function Devices({ isAdmin }: { isAdmin: boolean }) {
  const [devices, setDevices] = useState<DeviceRow[]>([])
  const [activationCode, setActivationCode] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [loadFailed, setLoadFailed] = useState(false)
  const [statusMessage, setStatusMessage] = useState<string | null>(null)

  const [testingDeviceId, setTestingDeviceId] = useState<string | null>(null)
  const [testReading, setTestReading] = useState({
    temperatureCelsius: '',
    humidityPercent: '',
    soilMoisturePercent: '',
    waterLevelPercent: '',
  })
  const [testStatus, setTestStatus] = useState<string | null>(null)

  const [discussionsDeviceId, setDiscussionsDeviceId] = useState<string | null>(null)
  const [discussions, setDiscussions] = useState<Discussion[]>([])
  const [discussionsError, setDiscussionsError] = useState<string | null>(null)
  const { t, formatDate } = useI18n()

  const load = () => {
    apiFetch('/devices')
      .then((res) => res.json())
      .then(setDevices)
      .then(() => setLoadFailed(false))
      .catch(() => setLoadFailed(true))
  }

  useEffect(load, [])

  const handlePair = async (e: React.FormEvent) => {
    e.preventDefault()
    setError(null)

    const res = await apiFetch('/devices/pair', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ activationCode }),
    })

    if (!res.ok) {
      setError(
        res.status === 409
          ? t('devices.alreadyPaired')
          : res.status === 404
            ? t('devices.unknownCode')
            : t('devices.pairFailed', { status: res.status }),
      )
      return
    }

    setActivationCode('')
    load()
  }

  const handleRename = async (id: string, name: string) => {
    const res = await apiFetch(`/devices/${id}`, {
      method: 'PATCH',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name }),
    })
    if (!res.ok) {
      setError(t('devices.renameFailed', { status: res.status }))
      return
    }
    load()
  }

  const handleDelete = async (id: string) => {
    setError(null)
    setStatusMessage(null)
    const name = devices.find((d) => d.id === id)?.name ?? t('devices.defaultName')
    const res = await apiFetch(`/devices/${id}`, { method: 'DELETE' })
    if (!res.ok) {
      setError(t('devices.removeFailed', { status: res.status }))
      return
    }
    // The server only unpairs (clears owner/schedules) rather than deleting
    // the row -- a physical device that's ever been activated stays visible
    // so it can be re-paired later. For a regular user that still means it
    // drops out of "My devices" (GET /devices filters non-admins by owner),
    // but an admin's "All devices" view intentionally keeps showing every
    // device regardless of owner, so the row remains after this succeeds --
    // just with its owner cleared. Without this message that reads as the
    // remove having silently failed.
    setStatusMessage(
      isAdmin ? t('devices.unpaired', { name }) : t('devices.removed', { name }),
    )
    load()
  }

  const handleToggleMuted = async (id: string, muted: boolean) => {
    const res = await apiFetch(`/devices/${id}/muted`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ muted }),
    })
    if (!res.ok) {
      setError(t('devices.muteFailed', { status: res.status }))
      return
    }
    load()
  }

  const toggleDiscussions = (id: string) => {
    if (discussionsDeviceId === id) {
      setDiscussionsDeviceId(null)
      return
    }

    setDiscussionsDeviceId(id)
    setDiscussions([])
    setDiscussionsError(null)
    apiFetch(`/devices/${id}/discussions`)
      .then((res) => {
        if (!res.ok) throw new Error()
        return res.json()
      })
      .then(setDiscussions)
      .catch(() => setDiscussionsError(t('discussions.loadFailed')))
  }

  const openTestForm = (id: string) => {
    setTestingDeviceId(id)
    setTestReading({ temperatureCelsius: '', humidityPercent: '', soilMoisturePercent: '', waterLevelPercent: '' })
    setTestStatus(null)
  }

  const handleSendTestReading = async (e: React.FormEvent, id: string) => {
    e.preventDefault()
    setTestStatus(t('devices.sending'))

    const toNumberOrNull = (v: string) => (v.trim() === '' ? null : Number(v))
    const res = await apiFetch(`/devices/${id}/test-reading`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        temperatureCelsius: toNumberOrNull(testReading.temperatureCelsius),
        humidityPercent: toNumberOrNull(testReading.humidityPercent),
        soilMoisturePercent: toNumberOrNull(testReading.soilMoisturePercent),
        waterLevelPercent: toNumberOrNull(testReading.waterLevelPercent),
      }),
    })

    if (res.ok) {
      setTestStatus(t('devices.sentReloading'))
      load()
    } else {
      setTestStatus(t('devices.failed', { status: res.status }))
    }
  }

  return (
    <section className="devices">
      <div className="page-header">
        <h2>{isAdmin ? t('devices.all') : t('devices.mine')}</h2>
        <span className="page-count">{devices.length}</span>
      </div>
      {isAdmin && (
        <form className="inline-form card" onSubmit={handlePair}>
          <input
            placeholder={t('devices.activationCode')}
            value={activationCode}
            onChange={(e) => setActivationCode(e.target.value)}
            required
          />
          <button type="submit">{t('devices.pair')}</button>
        </form>
      )}
      {loadFailed && <p className="form-error">{t('devices.loadFailed')}</p>}
      {error && <p className="form-error">{error}</p>}
      {statusMessage && <p className="form-status">{statusMessage}</p>}
      {devices.length === 0 && !error && !loadFailed && (
        <p className="empty-state">
          {isAdmin ? t('devices.emptyAdmin') : t('devices.emptyUser')}
        </p>
      )}
      <ul className="device-list">
        {devices.map((d) => (
          <li key={d.id} className="card device-card">
            <div className="device-header">
              <span className="device-avatar" aria-hidden="true">
                <LeafIcon />
                <span className={`device-online-dot ${d.isOnline ? 'online' : 'offline'}`} />
              </span>
              <input
                className="device-name"
                aria-label={t('devices.nameLabel')}
                defaultValue={d.name}
                onBlur={(e) => e.target.value !== d.name && handleRename(d.id, e.target.value)}
              />
              {isAdmin && <span className="chip">{d.owner ?? t('devices.noOwner')}</span>}
            </div>
            <div className="device-meta">
              <span className={d.isOnline ? 'device-status online' : 'device-status'}>
                {d.isOnline ? t('devices.online') : t('devices.offline')}
              </span>
              {d.muted && <span className="device-status muted">{t('devices.muted')}</span>}
              <span className="device-mac">{d.macAddress ?? '—'}</span>
              <span>{d.pairedAt ? t('devices.pairedAt', { date: formatDate(d.pairedAt) }) : t('devices.notPaired')}</span>
              {d.xiaozhiActivationCode && (
                <span className="device-mac" title="xiaozhi.me activation code">
                  xiaozhi: {d.xiaozhiActivationCode}
                </span>
              )}
            </div>
            <Telemetry reading={d.latestReading} />
            <div className="device-actions">
              <button type="button" onClick={() => toggleDiscussions(d.id)}>
                {discussionsDeviceId === d.id ? t('devices.hideConversations') : t('devices.conversations')}
              </button>
              <button type="button" onClick={() => handleToggleMuted(d.id, !d.muted)}>
                {d.muted ? t('devices.unmute') : t('devices.mute')}
              </button>
              {isAdmin && (
                <button type="button" onClick={() => openTestForm(d.id)}>
                  {t('devices.testReading')}
                </button>
              )}
              <button
                type="button"
                className="btn-danger"
                onClick={() => handleDelete(d.id)}
                title={
                  isAdmin ? t('devices.unpairHint') : t('devices.removeHint')
                }
              >
                {isAdmin ? t('devices.unpair') : t('devices.remove')}
              </button>
            </div>
            {isAdmin && testingDeviceId === d.id && (
              <form className="test-reading-form" onSubmit={(e) => handleSendTestReading(e, d.id)}>
                <input
                  type="number"
                  placeholder="Temperature °C"
                  value={testReading.temperatureCelsius}
                  onChange={(e) => setTestReading({ ...testReading, temperatureCelsius: e.target.value })}
                />
                <input
                  type="number"
                  placeholder="Humidity %"
                  value={testReading.humidityPercent}
                  onChange={(e) => setTestReading({ ...testReading, humidityPercent: e.target.value })}
                />
                <input
                  type="number"
                  placeholder="Soil moisture %"
                  value={testReading.soilMoisturePercent}
                  onChange={(e) => setTestReading({ ...testReading, soilMoisturePercent: e.target.value })}
                />
                <input
                  type="number"
                  placeholder="Water level %"
                  value={testReading.waterLevelPercent}
                  onChange={(e) => setTestReading({ ...testReading, waterLevelPercent: e.target.value })}
                />
                <button type="submit">{t('devices.send')}</button>
                <button type="button" onClick={() => setTestingDeviceId(null)}>
                  {t('devices.cancel')}
                </button>
                {testStatus && <span className="form-hint">{testStatus}</span>}
              </form>
            )}
            {discussionsDeviceId === d.id && (
              <Discussions deviceId={d.id} discussions={discussions} error={discussionsError} />
            )}
          </li>
        ))}
      </ul>
    </section>
  )
}

export function Discussions({
  deviceId,
  discussions,
  error,
}: {
  deviceId: string
  discussions: Discussion[]
  error: string | null
}) {
  const { t, formatDate } = useI18n()
  if (error) {
    return <p className="form-error">{error}</p>
  }

  if (discussions.length === 0) {
    return <p className="discussions discussions--empty">{t('discussions.empty')}</p>
  }

  return (
    <ul className="discussions">
      {discussions.map((m) => (
        <li key={m.id} className="discussion-turn">
          <p className="discussion-user">
            {m.userText}
            {m.hasUserAudio && <RecordingPlayer deviceId={deviceId} messageId={m.id} />}
          </p>
          <p className="discussion-assistant">{m.assistantText}</p>
          <span className="discussion-timestamp">{formatDate(m.createdAt)}</span>
        </li>
      ))}
    </ul>
  )
}

// Lazily fetches the user's recorded audio (rather than eagerly loading a WAV
// per turn on every history load) so the web console can compare what was
// actually said against what STT parsed it as -- see
// DiscussionMessage.UserAudioPath / LlmVoicePipeline. The blob URL is created
// once per click and reused for replays; revoked on unmount to avoid leaking
// it once the discussion list is hidden/reloaded.
function RecordingPlayer({ deviceId, messageId }: { deviceId: string; messageId: string }) {
  const [audioUrl, setAudioUrl] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [playerError, setPlayerError] = useState<string | null>(null)
  const { t } = useI18n()

  useEffect(() => {
    return () => {
      if (audioUrl) URL.revokeObjectURL(audioUrl)
    }
  }, [audioUrl])

  const handleLoad = async () => {
    setLoading(true)
    setPlayerError(null)
    try {
      const res = await apiFetch(`/devices/${deviceId}/discussions/${messageId}/audio`)
      if (!res.ok) throw new Error()
      const blob = await res.blob()
      setAudioUrl(URL.createObjectURL(blob))
    } catch {
      setPlayerError(t('discussions.playFailed'))
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
        {loading ? t('discussions.loading') : t('discussions.play')}
      </button>
      {playerError && <span className="form-error">{playerError}</span>}
    </span>
  )
}

function Telemetry({ reading }: { reading: Reading | null }) {
  const { t, formatDate } = useI18n()
  if (!reading) {
    return <p className="telemetry telemetry--empty">{t('telemetry.empty')}</p>
  }

  return (
    <>
      <dl className="telemetry">
        <div className="telemetry-tile telemetry-tile--temp">
          <dt>{t('telemetry.temperature')}</dt>
          <dd>{reading.temperatureCelsius != null ? `${reading.temperatureCelsius.toFixed(1)}°C` : '—'}</dd>
        </div>
        <div className="telemetry-tile telemetry-tile--humidity">
          <dt>{t('telemetry.humidity')}</dt>
          <dd>{reading.humidityPercent != null ? `${reading.humidityPercent.toFixed(0)}%` : '—'}</dd>
        </div>
        <div className="telemetry-tile telemetry-tile--soil">
          <dt>{t('telemetry.soil')}</dt>
          <dd>{reading.soilMoisturePercent != null ? `${reading.soilMoisturePercent.toFixed(0)}%` : '—'}</dd>
        </div>
        <div className="telemetry-tile telemetry-tile--water">
          <dt>{t('telemetry.water')}</dt>
          <dd>{reading.waterLevelPercent != null ? `${reading.waterLevelPercent.toFixed(0)}%` : '—'}</dd>
        </div>
      </dl>
      <p className="telemetry-timestamp">{t('telemetry.updated', { date: formatDate(reading.recordedAt) })}</p>
    </>
  )
}

function LeafIcon() {
  return (
    <svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor">
      <path d="M17 8C8 10 5.9 16.17 3.82 21.34l1.89.66.95-2.3c.48.17.98.3 1.34.3C19 20 22 3 22 3c-1 2-8 2.25-13 3.25S2 11.5 2 13.5s1.75 3.75 1.75 3.75C7 8 17 8 17 8z" />
    </svg>
  )
}

export default Devices
