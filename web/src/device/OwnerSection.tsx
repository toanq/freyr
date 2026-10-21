import { useState } from 'react'
import { useI18n } from '../i18n'
import type { Notify } from './DeviceDetail'
import OwnerPicker from './OwnerPicker'
import type { DeviceDetailDto } from './types'

type Props = {
  device: DeviceDetailDto
  onChanged: (patch: Partial<DeviceDetailDto>) => void
  notify: Notify
}

// Admin-only "Chủ sở hữu" card on the device page.
export default function OwnerSection({ device, onChanged, notify }: Props) {
  const { t, formatDate } = useI18n()
  const [open, setOpen] = useState(false)

  return (
    <section className="card owner-section">
      <div className="owner-section-row">
        <div className="owner-section-text">
          <h3>{t('owner.title')}</h3>
          <p>
            {device.owner ? (
              <>
                <strong>{device.owner}</strong>
                {device.pairedAt && <span className="form-hint"> · {t('devices.pairedAt', { date: formatDate(device.pairedAt) })}</span>}
              </>
            ) : (
              <span className="form-hint">{t('owner.none')}</span>
            )}
          </p>
        </div>
        <button type="button" onClick={() => setOpen(true)}>
          {device.owner ? t('owner.change') : t('owner.assignToAccount')}
        </button>
      </div>
      {open && (
        <OwnerPicker
          deviceId={device.id}
          deviceName={device.name}
          currentOwner={device.owner}
          onClose={() => setOpen(false)}
          onChanged={(updated) => {
            onChanged(updated)
            notify('success', updated.owner ? t('owner.assigned', { user: updated.owner }) : t('owner.cleared'))
          }}
        />
      )}
    </section>
  )
}
