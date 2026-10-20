import { useState } from 'react'
import { apiFetch } from '../api'
import { useI18n } from '../i18n'
import type { Notify } from './DeviceDetail'
import type { WateringSchedule } from './types'
import { DEFAULT_WATER_SECONDS, MAX_WATER_SECONDS, MIN_WATER_SECONDS, describeError, formatTime, isValidDuration } from './types'

// Same quick picks as the app's _presetTimes.
const PRESET_TIMES = ['06:00', '07:00', '12:00', '18:00', '20:00']

type Draft = { id: string | null; time: string; seconds: string; enabled: boolean }

type Props = {
  deviceId: string
  schedules: WateringSchedule[]
  reload: () => void
  notify: Notify
}

export default function Schedules({ deviceId, schedules, reload, notify }: Props) {
  const { t } = useI18n()
  const [draft, setDraft] = useState<Draft | null>(null)
  const [busy, setBusy] = useState(false)

  const send = async (path: string, method: string, body?: unknown) =>
    apiFetch(`/devices/${deviceId}/schedules${path}`, {
      method,
      headers: body ? { 'Content-Type': 'application/json' } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    })

  const save = async (e: React.FormEvent) => {
    e.preventDefault()
    if (!draft) return
    const match = /^(\d{1,2}):(\d{2})$/.exec(draft.time)
    const hour = match ? Number(match[1]) : NaN
    const minute = match ? Number(match[2]) : NaN
    if (!match || hour > 23 || minute > 59) {
      notify('error', t('dp.scheduleTimeInvalid'))
      return
    }
    const seconds = Number(draft.seconds)
    if (!isValidDuration(seconds)) {
      notify('error', t('dp.durationInvalid'))
      return
    }
    setBusy(true)
    try {
      const res = draft.id
        ? await send(`/${draft.id}`, 'PATCH', { hour, minute, durationSeconds: seconds, enabled: draft.enabled })
        : await send('', 'POST', { hour, minute, durationSeconds: seconds })
      if (!res.ok) {
        const key = draft.id ? 'dp.scheduleUpdateFailed' : 'dp.scheduleAddFailed'
        notify('error', t(key, { error: await describeError(res) }))
        return
      }
      setDraft(null)
      reload()
    } finally {
      setBusy(false)
    }
  }

  const toggle = async (s: WateringSchedule) => {
    const res = await send(`/${s.id}`, 'PATCH', {
      hour: s.hour,
      minute: s.minute,
      durationSeconds: s.durationSeconds,
      enabled: !s.enabled,
    })
    if (!res.ok) notify('error', t('dp.scheduleUpdateFailed', { error: await describeError(res) }))
    reload()
  }

  const remove = async (s: WateringSchedule) => {
    if (!window.confirm(t('dp.scheduleDeleteConfirm', { time: formatTime(s.hour, s.minute) }))) return
    const res = await send(`/${s.id}`, 'DELETE')
    if (!res.ok) notify('error', t('dp.scheduleDeleteFailed', { error: await describeError(res) }))
    if (draft?.id === s.id) setDraft(null)
    reload()
  }

  const openNew = () => setDraft({ id: null, time: '06:00', seconds: String(DEFAULT_WATER_SECONDS), enabled: true })
  const openEdit = (s: WateringSchedule) =>
    setDraft({ id: s.id, time: formatTime(s.hour, s.minute), seconds: String(s.durationSeconds), enabled: s.enabled })

  return (
    <section className="card dp-schedules">
      <div className="dp-section-head">
        <h3>{t('dp.schedules')}</h3>
        {!draft && (
          <button type="button" onClick={openNew}>
            + {t('dp.scheduleAdd')}
          </button>
        )}
      </div>

      {draft && (
        <form className="dp-schedule-form" onSubmit={save}>
          <h4>{draft.id ? t('dp.scheduleEdit') : t('dp.scheduleAdd')}</h4>
          <span className="dp-field-label">{t('dp.scheduleTime')}</span>
          <div className="dp-chips">
            {PRESET_TIMES.map((time) => (
              <button
                key={time}
                type="button"
                className={draft.time === time ? 'dp-chip dp-chip--active' : 'dp-chip'}
                aria-pressed={draft.time === time}
                onClick={() => setDraft({ ...draft, time })}
              >
                {time}
              </button>
            ))}
          </div>
          <div className="dp-form-row">
            <input
              type="time"
              aria-label={t('dp.scheduleTime')}
              value={draft.time}
              onChange={(e) => setDraft({ ...draft, time: e.target.value })}
              required
            />
            <label className="dp-inline-number">
              <input
                type="number"
                inputMode="numeric"
                min={MIN_WATER_SECONDS}
                max={MAX_WATER_SECONDS}
                aria-label={t('dp.waterSeconds')}
                value={draft.seconds}
                onChange={(e) => setDraft({ ...draft, seconds: e.target.value })}
                required
              />
              <span>{t('dp.scheduleSeconds', { seconds: '' }).trim()}</span>
            </label>
          </div>
          <div className="dp-form-actions">
            <button type="button" onClick={() => setDraft(null)} disabled={busy}>
              {t('dp.cancel')}
            </button>
            <button type="submit" disabled={busy}>
              {t('dp.save')}
            </button>
          </div>
        </form>
      )}

      {schedules.length === 0 && !draft && <p className="form-hint">{t('dp.schedulesEmpty')}</p>}
      <ul className="dp-schedule-list">
        {schedules.map((s) => {
          const time = formatTime(s.hour, s.minute)
          return (
            <li key={s.id} className={s.enabled ? 'dp-schedule' : 'dp-schedule dp-schedule--off'}>
              <button type="button" className="dp-schedule-main" onClick={() => openEdit(s)} title={t('dp.scheduleEdit')}>
                <span className="dp-schedule-time">{time}</span>
                <span className="dp-muted-text">{t('dp.scheduleSeconds', { seconds: s.durationSeconds })}</span>
              </button>
              <label className="dp-switch" title={t('dp.scheduleToggle', { time })}>
                <input
                  type="checkbox"
                  role="switch"
                  checked={s.enabled}
                  aria-label={t('dp.scheduleToggle', { time })}
                  onChange={() => toggle(s)}
                />
                <span aria-hidden="true" />
              </label>
              <button
                type="button"
                className="dp-icon-btn"
                aria-label={`${t('dp.scheduleDelete')} ${time}`}
                title={t('dp.scheduleDelete')}
                onClick={() => remove(s)}
              >
                ✕
              </button>
            </li>
          )
        })}
      </ul>
    </section>
  )
}
