import { useRef, useState } from 'react'
import { API_URL, apiFetch } from '../api'
import { useI18n } from '../i18n'
import type { DeviceDetailDto } from './types'
import { describeError } from './types'
import type { Notify } from './DeviceDetail'

const AVATAR_TYPES = ['image/jpeg', 'image/png', 'image/webp']
const MAX_AVATAR_BYTES = 8 * 1024 * 1024

type Props = {
  device: DeviceDetailDto
  isAdmin: boolean
  overallGood: boolean
  onChanged: (patch: Partial<DeviceDetailDto>) => void
  onDeleted: () => void
  notify: Notify
}

export default function DeviceHeader({ device, isAdmin, overallGood, onChanged, onDeleted, notify }: Props) {
  const { t } = useI18n()
  const fileInput = useRef<HTMLInputElement>(null)
  const [uploading, setUploading] = useState(false)
  // The avatar URL never changes between uploads, so bust the browser cache.
  const [avatarVersion, setAvatarVersion] = useState(0)
  const name = device.name || t('dp.defaultName')

  const rename = async () => {
    const next = window.prompt(t('dp.renamePrompt'), device.name)?.trim()
    if (!next || next === device.name) return
    const res = await apiFetch(`/devices/${device.id}`, {
      method: 'PATCH',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name: next }),
    })
    if (!res.ok) {
      notify('error', t('dp.renameFailed', { error: await describeError(res) }))
      return
    }
    onChanged({ name: next })
  }

  const remove = async () => {
    const message = isAdmin ? t('dp.unpairConfirm', { name }) : t('dp.deleteConfirm', { name })
    if (!window.confirm(message)) return
    const res = await apiFetch(`/devices/${device.id}`, { method: 'DELETE' })
    if (!res.ok) {
      notify('error', t('dp.deleteFailed', { error: await describeError(res) }))
      return
    }
    onDeleted()
  }

  const uploadAvatar = async (file: File) => {
    if (!AVATAR_TYPES.includes(file.type) || file.size === 0 || file.size > MAX_AVATAR_BYTES) {
      notify('error', t('dp.avatarType'))
      return
    }
    setUploading(true)
    try {
      const form = new FormData()
      form.append('file', file)
      const res = await apiFetch(`/devices/${device.id}/avatar`, { method: 'POST', body: form })
      if (!res.ok) {
        notify('error', t('dp.avatarFailed', { error: await describeError(res) }))
        return
      }
      const body: { avatarUrl: string } = await res.json()
      setAvatarVersion(Date.now())
      onChanged({ avatarUrl: body.avatarUrl })
    } catch (err) {
      notify('error', t('dp.avatarFailed', { error: String(err) }))
    } finally {
      setUploading(false)
    }
  }

  const updateFirmware = async () => {
    const res = await apiFetch('/admin/firmware')
    if (!res.ok) {
      notify('error', t('dp.fwLoadFailed', { error: await describeError(res) }))
      return
    }
    const builds: { id: string; version: string }[] = await res.json()
    if (builds.length === 0) {
      notify('info', t('dp.fwNone'))
      return
    }
    const latest = builds[0] // GET /admin/firmware is newest first
    if (latest.version === device.firmwareVersion) {
      notify('info', t('dp.fwUpToDate', { version: latest.version }))
      return
    }
    const message = t('dp.fwConfirm', { current: device.firmwareVersion ?? t('dp.fwUnknown'), latest: latest.version })
    if (!window.confirm(message)) return
    const ota = await apiFetch(`/devices/${device.id}/ota`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ firmwareBuildId: latest.id }),
    })
    if (ota.status === 503) notify('error', t('dp.fwFailed', { error: t('dp.brokerOffline') }))
    else if (!ota.ok) notify('error', t('dp.fwFailed', { error: await describeError(ota) }))
    else notify('success', t('dp.fwSent'))
  }

  const avatarSrc = device.avatarUrl ? `${API_URL}${device.avatarUrl}?v=${avatarVersion}` : null

  return (
    <header className="dp-header card">
      <button
        type="button"
        className="dp-avatar"
        onClick={() => fileInput.current?.click()}
        disabled={uploading}
        title={t('dp.avatarChange')}
        aria-label={t('dp.avatarChange')}
      >
        {avatarSrc ? <img src={avatarSrc} alt="" /> : <LeafGlyph />}
        <span className="dp-avatar-badge" aria-hidden="true">
          {uploading ? '…' : <CameraGlyph />}
        </span>
        <span className={`device-online-dot ${device.isOnline ? 'online' : 'offline'}`} />
      </button>
      <input
        ref={fileInput}
        type="file"
        accept="image/jpeg,image/png,image/webp"
        hidden
        onChange={(e) => {
          const file = e.target.files?.[0]
          e.target.value = ''
          if (file) uploadAvatar(file)
        }}
      />
      {uploading && <p className="form-hint">{t('dp.avatarUploading')}</p>}

      <div className="dp-title-row">
        <h2 className="dp-title">{name}</h2>
        {isAdmin && device.owner && <span className="chip">{device.owner}</span>}
      </div>

      <p className="dp-status-line">
        <span>{t('dp.status')}</span>{' '}
        <strong className={overallGood ? 'dp-tone-good' : 'dp-tone-low'}>
          {overallGood ? t('dp.statusGood') : t('dp.statusAttention')}
        </strong>
        <span className="dp-sep">·</span>
        <span className={device.isOnline ? 'device-status online' : 'device-status'}>
          {device.isOnline ? t('devices.online') : t('devices.offline')}
        </span>
        {device.firmwareVersion && (
          <>
            <span className="dp-sep">·</span>
            <span className="dp-muted-text">{t('dp.fwVersion', { version: device.firmwareVersion })}</span>
          </>
        )}
      </p>

      <div className="dp-header-actions">
        <button type="button" onClick={rename}>
          {t('dp.rename')}
        </button>
        {isAdmin && (
          <button type="button" onClick={updateFirmware}>
            {t('dp.fw')}
          </button>
        )}
        <button type="button" className="btn-danger" onClick={remove}>
          {isAdmin ? t('devices.unpair') : t('dp.delete')}
        </button>
      </div>
    </header>
  )
}

function CameraGlyph() {
  return (
    <svg viewBox="0 0 24 24" width="14" height="14" fill="currentColor" aria-hidden="true">
      <path d="M9 3 7.17 5H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V7a2 2 0 0 0-2-2h-3.17L15 3H9zm3 15a5 5 0 1 1 0-10 5 5 0 0 1 0 10zm0-2a3 3 0 1 0 0-6 3 3 0 0 0 0 6z" />
    </svg>
  )
}

function LeafGlyph() {
  return (
    <svg viewBox="0 0 24 24" width="56" height="56" fill="currentColor" aria-hidden="true">
      <path d="M17 8C8 10 5.9 16.17 3.82 21.34l1.89.66.95-2.3c.48.17.98.3 1.34.3C19 20 22 3 22 3c-1 2-8 2.25-13 3.25S2 11.5 2 13.5s1.75 3.75 1.75 3.75C7 8 17 8 17 8z" />
    </svg>
  )
}
