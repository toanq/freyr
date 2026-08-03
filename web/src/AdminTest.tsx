import { useEffect, useRef, useState } from 'react'
import { apiFetch } from './api'
import { Discussions, type Discussion } from './Devices'
import { startWavRecording, type WavRecorder } from './wavRecorder'

type DeviceOption = { id: string; name: string }

type MqttStatus =
  | { status: 'loading' }
  | { status: 'ok'; connected: boolean }
  | { status: 'error'; message: string }

function AdminTest() {
  const [devices, setDevices] = useState<DeviceOption[]>([])
  const [deviceId, setDeviceId] = useState<string>('')

  useEffect(() => {
    apiFetch('/devices')
      .then((res) => res.json())
      .then((data: DeviceOption[]) => {
        setDevices(data)
        if (data.length > 0) setDeviceId(data[0].id)
      })
      .catch(() => {})
  }, [])

  return (
    <section className="admin-test">
      <h2>Admin test tools</h2>

      <label className="admin-test-device-picker">
        Target device:{' '}
        <select value={deviceId} onChange={(e) => setDeviceId(e.target.value)}>
          {devices.length === 0 && <option value="">No devices</option>}
          {devices.map((d) => (
            <option key={d.id} value={d.id}>
              {d.name}
            </option>
          ))}
        </select>
      </label>

      <MqttHealth />
      <RemoteCommands deviceId={deviceId} />
      <SttTest />
      <TtsTest />
      <ChatTest deviceId={deviceId} />
    </section>
  )
}

function MqttHealth() {
  const [status, setStatus] = useState<MqttStatus>({ status: 'loading' })

  const check = () => {
    setStatus({ status: 'loading' })
    apiFetch('/admin/mqtt-status')
      .then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        return res.json()
      })
      .then((data: { connected: boolean }) => setStatus({ status: 'ok', connected: data.connected }))
      .catch((err) => setStatus({ status: 'error', message: String(err) }))
  }

  useEffect(check, [])

  return (
    <div className="admin-test-section">
      <h3>MQTT broker connection</h3>
      <p className={`health health--${status.status === 'ok' ? (status.connected ? 'ok' : 'error') : status.status}`}>
        {status.status === 'loading' && 'Checking…'}
        {status.status === 'ok' && (status.connected ? 'Connected to broker' : 'Not connected to broker')}
        {status.status === 'error' && `Could not check (${status.message})`}
      </p>
      <button type="button" onClick={check}>
        Refresh
      </button>
    </div>
  )
}

function RemoteCommands({ deviceId }: { deviceId: string }) {
  const [durationSeconds, setDurationSeconds] = useState('5')
  const [status, setStatus] = useState<string | null>(null)

  const sendWater = async () => {
    if (!deviceId) return
    setStatus('Sending water command…')
    const res = await apiFetch(`/devices/${deviceId}/water`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ durationSeconds: Number(durationSeconds) }),
    })
    setStatus(
      res.ok
        ? 'Water command sent -- check the device (fire-and-forget, no ack from the firmware).'
        : `Failed (HTTP ${res.status}).`,
    )
  }

  const sendPing = async () => {
    if (!deviceId) return
    setStatus('Sending ping command…')
    const res = await apiFetch(`/devices/${deviceId}/ping`, { method: 'POST' })
    setStatus(
      res.ok
        ? 'Ping command sent -- check the device (fire-and-forget, no ack from the firmware).'
        : `Failed (HTTP ${res.status}).`,
    )
  }

  return (
    <div className="admin-test-section">
      <h3>Remote commands</h3>
      <div className="admin-test-row">
        <input
          type="number"
          min={1}
          max={300}
          value={durationSeconds}
          onChange={(e) => setDurationSeconds(e.target.value)}
        />
        <span>seconds</span>
        <button type="button" onClick={sendWater} disabled={!deviceId}>
          Water now
        </button>
        <button type="button" onClick={sendPing} disabled={!deviceId}>
          Ping (find device)
        </button>
      </div>
      {status && <p className="admin-test-result">{status}</p>}
    </div>
  )
}

function SttTest() {
  const [file, setFile] = useState<File | null>(null)
  const [recordedBlob, setRecordedBlob] = useState<Blob | null>(null)
  const [recording, setRecording] = useState(false)
  const [status, setStatus] = useState<string | null>(null)
  const [text, setText] = useState<string | null>(null)
  const recorderRef = useRef<WavRecorder | null>(null)
  const [audioUrl, setAudioUrl] = useState<string | null>(null)

  const audioBlob = recordedBlob ?? file

  // Rebuild the object URL only when the underlying blob actually changes
  // (not on every render) and revoke the previous one -- createObjectURL
  // leaks memory otherwise.
  useEffect(() => {
    if (!audioBlob) {
      setAudioUrl(null)
      return
    }
    const url = URL.createObjectURL(audioBlob)
    setAudioUrl(url)
    return () => URL.revokeObjectURL(url)
  }, [audioBlob])

  const startRecording = async () => {
    setStatus(null)
    setText(null)
    setFile(null)
    setRecordedBlob(null)
    try {
      recorderRef.current = await startWavRecording()
      setRecording(true)
    } catch {
      setStatus('Could not access the microphone -- check browser permissions.')
    }
  }

  const stopRecording = async () => {
    if (!recorderRef.current) return
    const blob = await recorderRef.current.stop()
    recorderRef.current = null
    setRecording(false)
    setRecordedBlob(blob)
  }

  const transcribe = async () => {
    if (!audioBlob) return
    setStatus('Transcribing…')
    setText(null)

    const form = new FormData()
    form.append('file', audioBlob, 'recording.wav')
    const res = await apiFetch('/admin/voice-test/stt', { method: 'POST', body: form })

    if (!res.ok) {
      setStatus(`Failed (HTTP ${res.status}): ${await res.text()}`)
      return
    }
    const data: { text: string } = await res.json()
    setStatus(null)
    setText(data.text || '(empty transcript)')
  }

  return (
    <div className="admin-test-section">
      <h3>Speech-to-text test</h3>
      <p className="admin-test-hint">
        Record from your microphone, or upload a mono 16-bit PCM WAV file at 16000 Hz -- the exact format the
        firmware records.
      </p>
      <div className="admin-test-row">
        {!recording ? (
          <button type="button" onClick={startRecording}>
            Record
          </button>
        ) : (
          <button type="button" onClick={stopRecording}>
            Stop
          </button>
        )}
        <span>or</span>
        <input
          type="file"
          accept=".wav,audio/wav"
          onChange={(e) => {
            setRecordedBlob(null)
            setFile(e.target.files?.[0] ?? null)
          }}
        />
        <button type="button" onClick={transcribe} disabled={!audioBlob || recording}>
          Transcribe
        </button>
      </div>
      {audioUrl && !recording && (
        <audio className="admin-test-audio" controls src={audioUrl}>
          <track kind="captions" />
        </audio>
      )}
      {status && <p className="admin-test-result">{status}</p>}
      {text && <p className="admin-test-result admin-test-result--success">Transcript: "{text}"</p>}
    </div>
  )
}

function TtsTest() {
  const [text, setText] = useState('')
  const [status, setStatus] = useState<string | null>(null)
  const [audioUrl, setAudioUrl] = useState<string | null>(null)

  // Revoke the previous synthesized-audio URL whenever a new one replaces
  // it, or on unmount -- same reasoning as SttTest's recording preview.
  useEffect(() => {
    return () => {
      if (audioUrl) URL.revokeObjectURL(audioUrl)
    }
  }, [audioUrl])

  const synthesize = async () => {
    setStatus('Synthesizing…')
    const res = await apiFetch('/admin/voice-test/tts', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text }),
    })
    if (!res.ok) {
      setStatus(`Failed (HTTP ${res.status}): ${await res.text()}`)
      return
    }
    if (audioUrl) URL.revokeObjectURL(audioUrl)
    setAudioUrl(URL.createObjectURL(await res.blob()))
    setStatus(null)
  }

  return (
    <div className="admin-test-section">
      <h3>Text-to-speech test</h3>
      <p className="admin-test-hint">Runs the exact same ITextToSpeech (facebook/mms-tts-vie) a real voice reply uses.</p>
      <div className="admin-test-row">
        <input placeholder="Text to synthesize" value={text} onChange={(e) => setText(e.target.value)} />
        <button type="button" onClick={synthesize} disabled={!text.trim()}>
          Synthesize
        </button>
      </div>
      {status && <p className="admin-test-result">{status}</p>}
      {audioUrl && (
        <audio className="admin-test-audio" controls autoPlay src={audioUrl}>
          <track kind="captions" />
        </audio>
      )}
    </div>
  )
}

function ChatTest({ deviceId }: { deviceId: string }) {
  const [message, setMessage] = useState('')
  const [status, setStatus] = useState<string | null>(null)
  const [reply, setReply] = useState<string | null>(null)
  const [discussions, setDiscussions] = useState<Discussion[]>([])
  const [discussionsError, setDiscussionsError] = useState<string | null>(null)

  const loadHistory = () => {
    if (!deviceId) return
    setDiscussions([])
    setDiscussionsError(null)
    apiFetch(`/devices/${deviceId}/discussions`)
      .then((res) => {
        if (!res.ok) throw new Error()
        return res.json()
      })
      .then(setDiscussions)
      .catch(() => setDiscussionsError('Could not load conversation history.'))
  }

  useEffect(loadHistory, [deviceId])

  const send = async () => {
    if (!message.trim()) return
    setStatus('Sending…')
    setReply(null)
    const res = await apiFetch('/admin/voice-test/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message, deviceId: deviceId || null }),
    })
    if (!res.ok) {
      setStatus(`Failed (HTTP ${res.status}).`)
      return
    }
    const data: { reply: string } = await res.json()
    setStatus(null)
    setReply(data.reply)
    setMessage('')
    loadHistory() // if a deviceId was set, the exchange just got persisted -- refresh to show it
  }

  return (
    <div className="admin-test-section">
      <h3>Chat test (LLM only, no audio)</h3>
      <p className="admin-test-hint">
        Skips STT -- runs your typed message through the same LLM/system prompt a real voice turn uses. If a device
        is selected, the exchange is saved to that device's chat history below.
      </p>
      <div className="admin-test-row">
        <input
          placeholder="Type a message…"
          value={message}
          onChange={(e) => setMessage(e.target.value)}
          onKeyDown={(e) => e.key === 'Enter' && send()}
        />
        <button type="button" onClick={send} disabled={!message.trim()}>
          Send
        </button>
      </div>
      {status && <p className="admin-test-result">{status}</p>}
      {reply && <p className="admin-test-result admin-test-result--success">Assistant: "{reply}"</p>}

      <h4>Chat history</h4>
      <Discussions discussions={discussions} error={discussionsError} />
    </div>
  )
}

export default AdminTest
