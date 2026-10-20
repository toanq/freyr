import { useCallback, useEffect, useRef, useState } from 'react'
import { apiFetch } from '../api'
import { useI18n } from '../i18n'
import AssistantTalk from './AssistantTalk'
import DeviceHeader from './DeviceHeader'
import LowWaterAlerts from './LowWaterAlerts'
import Metrics from './Metrics'
import QuickActions from './QuickActions'
import Schedules from './Schedules'
import type { DeviceDetailDto, WateringSchedule } from './types'
import { describeError, metricStatuses } from './types'
import './DeviceDetail.css'

export type Notify = (kind: 'success' | 'error' | 'info', text: string) => void

type Load = { status: 'loading' } | { status: 'ready'; device: DeviceDetailDto } | { status: 'error'; message: string }

type Props = {
  deviceId: string
  isAdmin: boolean
  onBack: () => void
  onDeleted: () => void
}

export default function DeviceDetail({ deviceId, isAdmin, onBack, onDeleted }: Props) {
  const { t } = useI18n()
  const [load, setLoad] = useState<Load>({ status: 'loading' })
  const [schedules, setSchedules] = useState<WateringSchedule[]>([])
  const [notice, setNotice] = useState<{ kind: 'success' | 'error' | 'info'; text: string } | null>(null)
  const [talking, setTalking] = useState(false)
  const noticeTimer = useRef<number | null>(null)

  const notify = useCallback<Notify>((kind, text) => {
    setNotice({ kind, text })
    if (noticeTimer.current != null) window.clearTimeout(noticeTimer.current)
    noticeTimer.current = window.setTimeout(() => setNotice(null), kind === 'error' ? 8000 : 5000)
  }, [])

  useEffect(
    () => () => {
      if (noticeTimer.current != null) window.clearTimeout(noticeTimer.current)
    },
    [],
  )

  const loadDevice = useCallback(async () => {
    try {
      const res = await apiFetch(`/devices/${deviceId}`)
      if (res.status === 404) setLoad({ status: 'error', message: t('dp.notFound') })
      else if (!res.ok) setLoad({ status: 'error', message: t('dp.loadFailed', { error: await describeError(res) }) })
      else setLoad({ status: 'ready', device: await res.json() })
    } catch (err) {
      setLoad({ status: 'error', message: t('dp.loadFailed', { error: String(err) }) })
    }
  }, [deviceId, t])

  const loadSchedules = useCallback(async () => {
    const res = await apiFetch(`/devices/${deviceId}/schedules`).catch(() => null)
    if (res?.ok) setSchedules(await res.json())
  }, [deviceId])

  useEffect(() => {
    loadDevice()
    loadSchedules()
  }, [loadDevice, loadSchedules])

  const patchDevice = (patch: Partial<DeviceDetailDto>) =>
    setLoad((prev) => (prev.status === 'ready' ? { status: 'ready', device: { ...prev.device, ...patch } } : prev))

  return (
    <section className="device-detail">
      <div className="dp-topbar">
        <button type="button" className="dp-back" onClick={onBack}>
          ‹ {t('dp.back')}
        </button>
      </div>

      {notice && (
        <p className={`dp-notice dp-notice--${notice.kind}`} role={notice.kind === 'error' ? 'alert' : 'status'}>
          {notice.text}
        </p>
      )}

      {load.status === 'loading' && <p className="empty-state">{t('discussions.loading')}</p>}
      {load.status === 'error' && <p className="form-error">{load.message}</p>}
      {load.status === 'ready' && (
        <DetailBody
          device={load.device}
          isAdmin={isAdmin}
          schedules={schedules}
          talking={talking}
          setTalking={setTalking}
          patchDevice={patchDevice}
          reloadSchedules={loadSchedules}
          onDeleted={onDeleted}
          notify={notify}
        />
      )}
    </section>
  )
}

type BodyProps = {
  device: DeviceDetailDto
  isAdmin: boolean
  schedules: WateringSchedule[]
  talking: boolean
  setTalking: (v: boolean) => void
  patchDevice: (patch: Partial<DeviceDetailDto>) => void
  reloadSchedules: () => void
  onDeleted: () => void
  notify: Notify
}

function DetailBody(p: BodyProps) {
  const statuses = metricStatuses(p.device)
  const overallGood = Object.values(statuses).every((s) => s.tone === 'good')
  return (
    <>
      <DeviceHeader
        device={p.device}
        isAdmin={p.isAdmin}
        overallGood={overallGood}
        onChanged={p.patchDevice}
        onDeleted={p.onDeleted}
        notify={p.notify}
      />
      <QuickActions device={p.device} onChanged={p.patchDevice} onTalk={() => p.setTalking(true)} notify={p.notify} />
      {p.talking && <AssistantTalk deviceId={p.device.id} onClose={() => p.setTalking(false)} />}
      <Metrics device={p.device} />
      <Schedules deviceId={p.device.id} schedules={p.schedules} reload={p.reloadSchedules} notify={p.notify} />
      <LowWaterAlerts device={p.device} onChanged={p.patchDevice} notify={p.notify} />
    </>
  )
}
