import { useState } from 'react'
import { apiFetch } from '../api'
import { useI18n } from '../i18n'
import type { Notify } from './DeviceDetail'
import type { DeviceDetailDto } from './types'
import { describeError } from './types'

// Same presets as the app's _intervalPresets (minutes).
const INTERVAL_PRESETS = [60, 300, 600, 1440]

type Repeat = { kind: 'once' } | { kind: 'preset'; minutes: number } | { kind: 'custom' }

type Props = {
  device: DeviceDetailDto
  onChanged: (patch: Partial<DeviceDetailDto>) => void
  notify: Notify
}

const clampPercent = (text: string) => {
  const digits = text.replace(/[^0-9]/g, '')
  return digits === '' ? '' : String(Math.min(100, Number(digits)))
}

export default function LowWaterAlerts({ device, onChanged, notify }: Props) {
  const { t } = useI18n()
  const warning = device.lowWaterWarningThresholdPercent
  const critical = device.lowWaterCriticalThresholdPercent
  const interval = device.lowWaterAlertIntervalMinutes

  const [editing, setEditing] = useState(false)
  const [criticalText, setCriticalText] = useState('')
  const [warningText, setWarningText] = useState('')
  const [repeat, setRepeat] = useState<Repeat>({ kind: 'once' })
  const [customHours, setCustomHours] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)

  const intervalLabel = () => {
    if (interval == null) return ''
    return interval % 60 === 0
      ? t('dp.alertsEveryHours', { hours: interval / 60 })
      : t('dp.alertsEveryMinutes', { minutes: interval })
  }

  const summary =
    warning == null && critical == null
      ? t('dp.alertsOff')
      : [
          warning != null ? t('dp.alertsWarning', { percent: warning }) : null,
          critical != null ? t('dp.alertsCritical', { percent: critical }) + intervalLabel() : null,
        ]
          .filter(Boolean)
          .join(' · ')

  const open = () => {
    setCriticalText(String(critical ?? 0))
    setWarningText(String(warning ?? 0))
    const isPreset = interval != null && INTERVAL_PRESETS.includes(interval)
    setRepeat(interval == null ? { kind: 'once' } : isPreset ? { kind: 'preset', minutes: interval } : { kind: 'custom' })
    setCustomHours(interval != null && !isPreset ? String(Math.round(interval / 60)) : '')
    setError(null)
    setEditing(true)
  }

  const onCriticalChange = (text: string) => {
    const next = clampPercent(text)
    setCriticalText(next)
    // Warning must never sit below critical -- raise it live, like the app.
    const c = Number(next || 0)
    if (c !== 0 && Number(warningText || 0) < c) setWarningText(String(c))
  }

  const save = async (e: React.FormEvent) => {
    e.preventDefault()
    let w = Number(warningText || 0)
    const c = Number(criticalText || 0)
    if (w < 0 || w > 100 || c < 0 || c > 100) {
      setError(t('dp.alertsRange'))
      return
    }
    if (w !== 0 && c !== 0 && w < c) w = c
    let intervalMinutes: number | null = null
    if (c !== 0) {
      if (repeat.kind === 'custom') {
        const hours = Number(customHours)
        if (!Number.isInteger(hours) || hours < 1) {
          setError(t('dp.alertsHoursInvalid'))
          return
        }
        intervalMinutes = hours * 60
      } else if (repeat.kind === 'preset') {
        intervalMinutes = repeat.minutes
      }
    }
    const patch = {
      lowWaterWarningThresholdPercent: w === 0 ? null : w,
      lowWaterCriticalThresholdPercent: c === 0 ? null : c,
      lowWaterAlertIntervalMinutes: intervalMinutes,
    }
    setSaving(true)
    try {
      const res = await apiFetch(`/devices/${device.id}/alerts`, {
        method: 'PATCH',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(patch),
      })
      if (!res.ok) {
        notify('error', t('dp.alertsFailed', { error: await describeError(res) }))
        return
      }
      onChanged(patch)
      setEditing(false)
    } finally {
      setSaving(false)
    }
  }

  if (!editing) {
    return (
      <button type="button" className="card dp-alerts-summary" onClick={open}>
        <span>
          <strong>{t('dp.alerts')}</strong>
          <span className="dp-muted-text">{summary}</span>
        </span>
        <span aria-hidden="true">›</span>
      </button>
    )
  }

  const criticalOff = Number(criticalText || 0) === 0
  const chip = (active: boolean, label: string, onClick: () => void) => (
    <button key={label} type="button" className={active ? 'dp-chip dp-chip--active' : 'dp-chip'} aria-pressed={active} onClick={onClick}>
      {label}
    </button>
  )

  return (
    <form className="card dp-alerts-form" onSubmit={save}>
      <h3>{t('dp.alerts')}</h3>
      <label className="dp-field">
        <span className="dp-field-label">{t('dp.alertsCriticalLabel')}</span>
        <span className="dp-percent">
          <input inputMode="numeric" value={criticalText} onChange={(e) => onCriticalChange(e.target.value)} />%
        </span>
        <span className="form-hint">{t('dp.alertsOffHint')}</span>
      </label>
      <label className="dp-field">
        <span className="dp-field-label">{t('dp.alertsWarningLabel')}</span>
        <span className="dp-percent">
          <input inputMode="numeric" value={warningText} onChange={(e) => setWarningText(clampPercent(e.target.value))} />%
        </span>
        <span className="form-hint">{criticalOff ? t('dp.alertsOffHint') : t('dp.alertsMinHint')}</span>
      </label>
      <span className="dp-field-label">{t('dp.alertsRepeat')}</span>
      <div className="dp-chips">
        {chip(repeat.kind === 'once', t('dp.alertsOnce'), () => setRepeat({ kind: 'once' }))}
        {INTERVAL_PRESETS.map((minutes) =>
          chip(repeat.kind === 'preset' && repeat.minutes === minutes, t('dp.alertsHours', { hours: minutes / 60 }), () =>
            setRepeat({ kind: 'preset', minutes }),
          ),
        )}
        {chip(repeat.kind === 'custom', t('dp.alertsCustom'), () => setRepeat({ kind: 'custom' }))}
      </div>
      {repeat.kind === 'custom' && (
        <label className="dp-field">
          <span className="dp-field-label">{t('dp.alertsCustomLabel')}</span>
          <input
            type="number"
            inputMode="numeric"
            min={1}
            value={customHours}
            onChange={(e) => setCustomHours(e.target.value)}
            autoFocus
          />
        </label>
      )}
      {error && <p className="form-error">{error}</p>}
      <div className="dp-form-actions">
        <button type="button" onClick={() => setEditing(false)} disabled={saving}>
          {t('dp.cancel')}
        </button>
        <button type="submit" disabled={saving}>
          {t('dp.save')}
        </button>
      </div>
    </form>
  )
}
