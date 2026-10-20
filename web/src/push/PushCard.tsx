import { useEffect, useState } from 'react'
import { useI18n } from '../i18n'
import { disablePush, enablePush, getPushStatus, isPushConfigured, type PushStatus } from './pushClient'
import './push.css'

// Opt-in control for web push; renders nothing when Firebase isn't configured.
function PushCard() {
  const { t } = useI18n()
  const [status, setStatus] = useState<PushStatus | null>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    if (!isPushConfigured()) return
    let cancelled = false
    getPushStatus()
      .then((s) => !cancelled && setStatus(s))
      .catch(() => !cancelled && setStatus('unsupported'))
    return () => {
      cancelled = true
    }
  }, [])

  if (!isPushConfigured() || status === null) return null

  const toggle = async () => {
    setBusy(true)
    setError(null)
    try {
      if (status === 'on') {
        await disablePush()
        setStatus('off')
      } else {
        setStatus(await enablePush())
      }
    } catch (err) {
      setError(t('push.failed', { error: String(err) }))
    } finally {
      setBusy(false)
    }
  }

  const hint =
    status === 'needs-install'
      ? t('push.needsInstall')
      : status === 'unsupported'
        ? t('push.unsupported')
        : status === 'blocked'
          ? t('push.blocked')
          : status === 'on'
            ? t('push.onHint')
            : t('push.offHint')

  return (
    <div className="card push-card">
      <div className="push-card-row">
        <strong>{t('push.title')}</strong>
        {(status === 'on' || status === 'off') && (
          <button
            type="button"
            className={status === 'on' ? undefined : 'btn-primary'}
            onClick={toggle}
            disabled={busy}
            aria-pressed={status === 'on'}
          >
            {busy ? t('push.working') : status === 'on' ? t('push.disable') : t('push.enable')}
          </button>
        )}
      </div>
      <p className="form-hint">{hint}</p>
      {error && <p className="form-error">{error}</p>}
    </div>
  )
}

export default PushCard
