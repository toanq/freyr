import { useCallback, useEffect, useRef, useState } from 'react'
import { apiFetch } from '../api'
import { useI18n } from '../i18n'
import { startWavRecording, type WavRecorder } from '../wavRecorder'
import { describeError } from './types'

export type TalkState = 'idle' | 'listening' | 'processing' | 'speaking'
export type TalkTurn = { you: string; synora: string }

// Timings mirror the app's AssistantTalkSheet.
const WHOLE_SESSION_SILENCE_MS = 10_000
const TRAILING_SILENCE_MS = 1_500
const CALIBRATION_MS = 800
const SPEECH_MARGIN_DB = 12
const QUIET_DB = -50
const LOUD_DB = -10
const REQUEST_TIMEOUT_MS = 90_000
const GOODBYE_PHRASES = ['tạm biệt', 'chào tạm biệt', 'hẹn gặp lại', 'goodbye', 'bye bye', 'bye', 'bai bai', 'bái bai', 'bai bye']

const isGoodbye = (text: string | null | undefined) =>
  !!text && GOODBYE_PHRASES.some((p) => text.toLowerCase().includes(p))

export function useAssistantTalk(deviceId: string) {
  const { t } = useI18n()
  const [state, setStateValue] = useState<TalkState>('idle')
  const [scale, setScale] = useState(1)
  const [turns, setTurns] = useState<TalkTurn[]>([])
  const [error, setError] = useState<string | null>(null)

  const stateRef = useRef<TalkState>('idle')
  const recorderRef = useRef<WavRecorder | null>(null)
  const timerRef = useRef<number | null>(null)
  const abortRef = useRef<AbortController | null>(null)
  const audioRef = useRef<HTMLAudioElement | null>(null)
  const audioUrlRef = useRef<string | null>(null)
  const endAfterReplyRef = useRef(false)
  const level = useRef({ startedAt: 0, calibration: [] as number[], floor: null as number | null, hasSpeech: false, lastSpeechAt: 0 })

  const setState = useCallback((next: TalkState) => {
    stateRef.current = next
    setStateValue(next)
  }, [])

  const clearTimer = () => {
    if (timerRef.current != null) window.clearInterval(timerRef.current)
    timerRef.current = null
  }

  const releaseAudio = () => {
    audioRef.current?.pause()
    audioRef.current = null
    if (audioUrlRef.current) URL.revokeObjectURL(audioUrlRef.current)
    audioUrlRef.current = null
  }

  const discardRecorder = () => {
    const recorder = recorderRef.current
    recorderRef.current = null
    recorder?.stop().catch(() => {})
  }

  const stop = useCallback(() => {
    clearTimer()
    abortRef.current?.abort()
    abortRef.current = null
    discardRecorder()
    releaseAudio()
    setScale(1)
    setState('idle')
  }, [setState])

  const onLevel = (db: number) => {
    if (stateRef.current !== 'listening') return
    const clamped = Math.min(LOUD_DB, Math.max(QUIET_DB, db))
    setScale(0.7 + ((clamped - QUIET_DB) / (LOUD_DB - QUIET_DB)) * 0.6)
    const l = level.current
    const now = performance.now()
    // Calibrate this room's noise floor first, like the app, instead of a fixed dBFS threshold.
    if (l.floor == null) {
      l.calibration.push(db)
      if (now - l.startedAt >= CALIBRATION_MS && l.calibration.length > 0) {
        l.floor = l.calibration.reduce((a, b) => a + b, 0) / l.calibration.length
      }
      return
    }
    if (db > l.floor + SPEECH_MARGIN_DB) {
      l.hasSpeech = true
      l.lastSpeechAt = now
    }
  }

  // Declared as a ref-held function so the silence timer and audio callbacks
  // always call the latest version without re-subscribing.
  const flow = useRef({ startListening: async () => {}, finish: async () => {} })

  const speak = async (base64: string | null) => {
    setState('speaking')
    const next = () => {
      if (stateRef.current !== 'speaking') return
      if (endAfterReplyRef.current) stop()
      else flow.current.startListening()
    }
    if (!base64) {
      window.setTimeout(next, 2000)
      return
    }
    try {
      const bytes = Uint8Array.from(atob(base64), (c) => c.charCodeAt(0))
      releaseAudio()
      const url = URL.createObjectURL(new Blob([bytes], { type: 'audio/wav' }))
      audioUrlRef.current = url
      const audio = new Audio(url)
      audioRef.current = audio
      audio.onended = () => {
        releaseAudio()
        next()
      }
      await audio.play()
    } catch (err) {
      setError(t('talk.playFailed', { error: String(err) }))
      releaseAudio()
      next()
    }
  }

  const send = async (wav: Blob) => {
    setState('processing')
    const controller = new AbortController()
    abortRef.current = controller
    let timedOut = false
    const timeout = window.setTimeout(() => {
      timedOut = true
      controller.abort()
    }, REQUEST_TIMEOUT_MS)
    try {
      const form = new FormData()
      form.append('file', wav, 'recording.wav')
      const res = await apiFetch(`/devices/${deviceId}/assistant/turn`, { method: 'POST', body: form, signal: controller.signal })
      if (!res.ok) throw new Error(await describeError(res))
      const body: { transcript: string; reply: string; replyAudioWavBase64: string | null } = await res.json()
      if (stateRef.current !== 'processing') return
      setTurns((prev) => [...prev, { you: body.transcript, synora: body.reply }])
      endAfterReplyRef.current = isGoodbye(body.transcript)
      await speak(body.replyAudioWavBase64)
    } catch (err) {
      if (controller.signal.aborted && !timedOut) return // Stop pressed
      setError(timedOut ? t('talk.timeout') : t('talk.failed', { error: err instanceof Error ? err.message : String(err) }))
      stop()
    } finally {
      window.clearTimeout(timeout)
      if (abortRef.current === controller) abortRef.current = null
    }
  }

  flow.current.finish = async () => {
    clearTimer()
    const recorder = recorderRef.current
    recorderRef.current = null
    if (!recorder) return stop()
    const wav = await recorder.stop()
    if (stateRef.current !== 'listening') return
    await send(wav)
  }

  flow.current.startListening = async () => {
    setState('listening')
    setScale(1)
    level.current = { startedAt: performance.now(), calibration: [], floor: null, hasSpeech: false, lastSpeechAt: 0 }
    try {
      const recorder = await startWavRecording({ onLevel })
      if (stateRef.current !== 'listening') {
        recorder.stop().catch(() => {})
        return
      }
      recorderRef.current = recorder
    } catch (err) {
      const denied = err instanceof DOMException && (err.name === 'NotAllowedError' || err.name === 'SecurityError')
      setError(denied ? t('talk.micDenied') : t('talk.recordFailed', { error: String(err) }))
      stop()
      return
    }
    clearTimer()
    timerRef.current = window.setInterval(() => {
      if (stateRef.current !== 'listening') return
      const l = level.current
      const now = performance.now()
      if (!l.hasSpeech) {
        if (now - l.startedAt >= WHOLE_SESSION_SILENCE_MS) stop()
        return
      }
      if (now - l.lastSpeechAt >= TRAILING_SILENCE_MS) flow.current.finish()
    }, 200)
  }

  const start = useCallback(() => {
    setError(null)
    if (!navigator.mediaDevices?.getUserMedia) {
      setError(t('talk.insecure'))
      return
    }
    endAfterReplyRef.current = false
    flow.current.startListening()
  }, [t])

  useEffect(() => stop, [stop])

  return { state, scale, turns, error, start, stop, micAvailable: !!navigator.mediaDevices?.getUserMedia }
}
