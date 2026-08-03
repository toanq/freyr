import { useEffect, useState } from 'react'
import { apiFetch } from './api'

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
}

function Devices({ isAdmin }: { isAdmin: boolean }) {
  const [devices, setDevices] = useState<DeviceRow[]>([])
  const [activationCode, setActivationCode] = useState('')
  const [error, setError] = useState<string | null>(null)

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

  const load = () => {
    apiFetch('/devices')
      .then((res) => res.json())
      .then(setDevices)
      .catch(() => setError('Could not load devices.'))
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
          ? 'Device already paired.'
          : res.status === 404
            ? 'Unknown activation code.'
            : `Could not pair device (HTTP ${res.status}).`,
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
      setError(`Could not rename device (HTTP ${res.status}).`)
      return
    }
    load()
  }

  const handleDelete = async (id: string) => {
    const res = await apiFetch(`/devices/${id}`, { method: 'DELETE' })
    if (!res.ok) {
      setError(`Could not remove device (HTTP ${res.status}).`)
      return
    }
    load()
  }

  const handleToggleMuted = async (id: string, muted: boolean) => {
    const res = await apiFetch(`/devices/${id}/muted`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ muted }),
    })
    if (!res.ok) {
      setError(`Could not update muted state (HTTP ${res.status}).`)
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
      .catch(() => setDiscussionsError('Could not load conversation history.'))
  }

  const openTestForm = (id: string) => {
    setTestingDeviceId(id)
    setTestReading({ temperatureCelsius: '', humidityPercent: '', soilMoisturePercent: '', waterLevelPercent: '' })
    setTestStatus(null)
  }

  const handleSendTestReading = async (e: React.FormEvent, id: string) => {
    e.preventDefault()
    setTestStatus('Sending…')

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
      setTestStatus('Sent -- reloading…')
      load()
    } else {
      setTestStatus(`Failed (HTTP ${res.status}).`)
    }
  }

  return (
    <section className="devices">
      <h2>{isAdmin ? 'All devices' : 'My devices'}</h2>
      <form onSubmit={handlePair}>
        <input
          placeholder="Activation code"
          value={activationCode}
          onChange={(e) => setActivationCode(e.target.value)}
          required
        />
        <button type="submit">Pair device</button>
      </form>
      {error && <p className="form-error">{error}</p>}
      <ul>
        {devices.map((d) => (
          <li key={d.id}>
            <div className="device-row">
              <input
                defaultValue={d.name}
                onBlur={(e) => e.target.value !== d.name && handleRename(d.id, e.target.value)}
              />
              {isAdmin && <span className="device-owner">{d.owner ?? 'unpaired'}</span>}
              <span className="device-mac">{d.macAddress ?? '—'}</span>
              <span>{d.pairedAt ? `Paired ${new Date(d.pairedAt).toLocaleString()}` : 'Not paired'}</span>
              <span className={`device-online-dot ${d.isOnline ? 'online' : 'offline'}`} />
              <span>{d.isOnline ? 'Online' : 'Offline'}</span>
              {d.xiaozhiActivationCode && (
                <span className="device-xiaozhi-code" title="xiaozhi.me activation code">
                  xiaozhi: {d.xiaozhiActivationCode}
                </span>
              )}
              {isAdmin && (
                <button type="button" onClick={() => openTestForm(d.id)}>
                  Send test reading
                </button>
              )}
              <button type="button" onClick={() => handleToggleMuted(d.id, !d.muted)}>
                {d.muted ? 'Unmute' : 'Mute'}
              </button>
              <button type="button" onClick={() => toggleDiscussions(d.id)}>
                {discussionsDeviceId === d.id ? 'Hide conversations' : 'Show conversations'}
              </button>
              <button type="button" onClick={() => handleDelete(d.id)}>
                Remove
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
                <button type="submit">Send</button>
                <button type="button" onClick={() => setTestingDeviceId(null)}>
                  Cancel
                </button>
                {testStatus && <span> {testStatus}</span>}
              </form>
            )}
            <Telemetry reading={d.latestReading} />
            {discussionsDeviceId === d.id && (
              <Discussions discussions={discussions} error={discussionsError} />
            )}
          </li>
        ))}
      </ul>
    </section>
  )
}

export function Discussions({ discussions, error }: { discussions: Discussion[]; error: string | null }) {
  if (error) {
    return <p className="form-error">{error}</p>
  }

  if (discussions.length === 0) {
    return <p className="discussions discussions--empty">No conversations yet.</p>
  }

  return (
    <ul className="discussions">
      {discussions.map((m) => (
        <li key={m.id} className="discussion-turn">
          <p className="discussion-user">{m.userText}</p>
          <p className="discussion-assistant">{m.assistantText}</p>
          <span className="discussion-timestamp">{new Date(m.createdAt).toLocaleString()}</span>
        </li>
      ))}
    </ul>
  )
}

function Telemetry({ reading }: { reading: Reading | null }) {
  if (!reading) {
    return <p className="telemetry telemetry--empty">No sensor data yet.</p>
  }

  return (
    <dl className="telemetry">
      <div>
        <dt>Temperature</dt>
        <dd>{reading.temperatureCelsius != null ? `${reading.temperatureCelsius.toFixed(1)} °C` : '—'}</dd>
      </div>
      <div>
        <dt>Humidity</dt>
        <dd>{reading.humidityPercent != null ? `${reading.humidityPercent.toFixed(0)}%` : '—'}</dd>
      </div>
      <div>
        <dt>Soil moisture</dt>
        <dd>{reading.soilMoisturePercent != null ? `${reading.soilMoisturePercent.toFixed(0)}%` : '—'}</dd>
      </div>
      <div>
        <dt>Water level</dt>
        <dd>{reading.waterLevelPercent != null ? `${reading.waterLevelPercent.toFixed(0)}%` : '—'}</dd>
      </div>
      <div className="telemetry-timestamp">
        <dt>Updated</dt>
        <dd>{new Date(reading.recordedAt).toLocaleString()}</dd>
      </div>
    </dl>
  )
}

export default Devices
