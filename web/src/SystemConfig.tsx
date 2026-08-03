import { useEffect, useState } from 'react'
import { apiFetch } from './api'

type TtsVoice = { id: string; description: string; gender: string }
type LoadState = { status: 'loading' } | { status: 'ready' } | { status: 'error'; message: string }

const PREVIEW_TEXT = 'Xin chào, tôi là Synora. Đây là giọng nói hiện tại.'

function SystemConfig() {
  const [load, setLoad] = useState<LoadState>({ status: 'loading' })
  const [voices, setVoices] = useState<TtsVoice[]>([])
  const [ttsVoice, setTtsVoice] = useState('')
  const [status, setStatus] = useState<string | null>(null)
  const [previewUrl, setPreviewUrl] = useState<string | null>(null)

  useEffect(() => {
    Promise.all([
      apiFetch('/admin/system-settings').then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        return res.json()
      }),
      apiFetch('/admin/tts-voices').then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        return res.json()
      }),
    ])
      .then(([settings, voiceList]: [{ ttsVoice: string }, TtsVoice[]]) => {
        setTtsVoice(settings.ttsVoice)
        setVoices(voiceList)
        setLoad({ status: 'ready' })
      })
      .catch((err) => setLoad({ status: 'error', message: String(err) }))
  }, [])

  useEffect(() => {
    return () => {
      if (previewUrl) URL.revokeObjectURL(previewUrl)
    }
  }, [previewUrl])

  const save = async (e: React.FormEvent) => {
    e.preventDefault()
    setStatus('Saving…')
    const res = await apiFetch('/admin/system-settings', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ttsVoice }),
    })
    if (!res.ok) {
      setStatus(`Failed (HTTP ${res.status}): ${await res.text()}`)
      return
    }
    setStatus('Saved -- takes effect on the next voice reply, no restart needed.')
  }

  const preview = async () => {
    setStatus('Generating preview (saves the selection first)…')
    const saveRes = await apiFetch('/admin/system-settings', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ttsVoice }),
    })
    if (!saveRes.ok) {
      setStatus(`Failed (HTTP ${saveRes.status}): ${await saveRes.text()}`)
      return
    }
    const res = await apiFetch('/admin/voice-test/tts', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: PREVIEW_TEXT }),
    })
    if (!res.ok) {
      setStatus(`Failed (HTTP ${res.status}): ${await res.text()}`)
      return
    }
    if (previewUrl) URL.revokeObjectURL(previewUrl)
    setPreviewUrl(URL.createObjectURL(await res.blob()))
    setStatus('Saved and previewed.')
  }

  if (load.status === 'loading') return <p>Loading…</p>
  if (load.status === 'error') return <p className="form-error">Could not load settings ({load.message}).</p>

  return (
    <section className="system-config">
      <h2>System config</h2>
      <p className="admin-test-hint">
        Global settings that apply to every device. Changes take effect immediately -- no redeploy or restart.
      </p>

      <form className="admin-test-section" onSubmit={save}>
        <h3>TTS voice</h3>
        <p className="admin-test-hint">
          One of VieNeu-TTS v3 Turbo's 14 built-in preset voices -- no reference audio needed. This list is fetched
          live from the TTS sidecar, so it can't drift out of sync with what's actually available.
        </p>
        <div className="admin-test-row">
          <select value={ttsVoice} onChange={(e) => setTtsVoice(e.target.value)}>
            {voices.map((v) => (
              <option key={v.id} value={v.id}>
                {v.id} — {v.description}
              </option>
            ))}
          </select>
          <button type="submit">Save</button>
          <button type="button" onClick={preview}>
            Save &amp; preview
          </button>
        </div>
        {status && <p className="admin-test-result">{status}</p>}
        {previewUrl && (
          <audio className="admin-test-audio" controls autoPlay src={previewUrl}>
            <track kind="captions" />
          </audio>
        )}
      </form>
    </section>
  )
}

export default SystemConfig
