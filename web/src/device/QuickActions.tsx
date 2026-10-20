import { useState } from 'react'
import { apiFetch } from '../api'
import { useI18n } from '../i18n'
import type { Notify } from './DeviceDetail'
import type { DeviceDetailDto } from './types'
import { DEFAULT_WATER_SECONDS, MAX_WATER_SECONDS, MIN_WATER_SECONDS, describeError, isValidDuration } from './types'

type Props = {
  device: DeviceDetailDto
  onChanged: (patch: Partial<DeviceDetailDto>) => void
  onTalk: () => void
  notify: Notify
}

export default function QuickActions({ device, onChanged, onTalk, notify }: Props) {
  const { t } = useI18n()
  const [seconds, setSeconds] = useState(String(DEFAULT_WATER_SECONDS))
  const [watering, setWatering] = useState(false)
  const [pinging, setPinging] = useState(false)
  const [togglingMute, setTogglingMute] = useState(false)

  const commandError = async (res: Response) =>
    res.status === 503 ? t('dp.brokerOffline') : await describeError(res)

  const water = async () => {
    const value = Number(seconds)
    if (!isValidDuration(value)) {
      notify('error', t('dp.durationInvalid'))
      return
    }
    if (!window.confirm(t('dp.waterConfirm', { seconds: value }))) return
    setWatering(true)
    try {
      const res = await apiFetch(`/devices/${device.id}/water`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ durationSeconds: value }),
      })
      if (res.ok) notify('success', t('dp.waterSent'))
      else notify('error', t('dp.waterFailed', { error: await commandError(res) }))
    } catch (err) {
      notify('error', t('dp.waterFailed', { error: String(err) }))
    } finally {
      setWatering(false)
    }
  }

  const ping = async () => {
    setPinging(true)
    try {
      const res = await apiFetch(`/devices/${device.id}/ping`, { method: 'POST' })
      if (res.ok) notify('success', t('dp.pingSent'))
      else notify('error', t('dp.pingFailed', { error: await commandError(res) }))
    } catch (err) {
      notify('error', t('dp.pingFailed', { error: String(err) }))
    } finally {
      setPinging(false)
    }
  }

  const toggleMute = async () => {
    setTogglingMute(true)
    try {
      const res = await apiFetch(`/devices/${device.id}/muted`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ muted: !device.muted }),
      })
      if (res.ok) onChanged({ muted: !device.muted })
      else notify('error', t('dp.muteFailed', { error: await describeError(res) }))
    } catch (err) {
      notify('error', t('dp.muteFailed', { error: String(err) }))
    } finally {
      setTogglingMute(false)
    }
  }

  return (
    <section className="dp-actions card">
      <div className="dp-water-row">
        <label className="dp-water-seconds">
          <span>{t('dp.waterSeconds')}</span>
          <input
            type="number"
            inputMode="numeric"
            min={MIN_WATER_SECONDS}
            max={MAX_WATER_SECONDS}
            value={seconds}
            onChange={(e) => setSeconds(e.target.value)}
          />
        </label>
        <button type="button" className="btn-primary dp-water-btn" onClick={water} disabled={watering}>
          {watering ? '…' : t('dp.waterNow')}
        </button>
      </div>
      <div className="dp-action-row">
        <button type="button" onClick={ping} disabled={pinging}>
          {pinging ? '…' : t('dp.ping')}
        </button>
        <button
          type="button"
          className={device.muted ? 'dp-toggle-on' : undefined}
          aria-pressed={device.muted}
          onClick={toggleMute}
          disabled={togglingMute}
        >
          {device.muted ? t('dp.muted') : t('dp.mute')}
        </button>
      </div>
      {!device.isOnline && <p className="form-hint">{t('dp.offlineHint')}</p>}
      <button type="button" className="btn-primary dp-talk-btn" onClick={onTalk}>
        {t('talk.open')}
      </button>
    </section>
  )
}
