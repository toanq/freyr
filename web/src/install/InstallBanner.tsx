import { useEffect, useState } from 'react'
import { useI18n } from '../i18n'
import { isIos, isStandalone } from '../platform'
import { canPromptInstall, promptInstall, subscribeInstall, wasJustInstalled } from './installPrompt'
import './install.css'

const DISMISS_KEY = 'synora.installDismissedAt'
const DISMISS_DAYS = 14

function recentlyDismissed(): boolean {
  try {
    const at = Number(localStorage.getItem(DISMISS_KEY))
    return Number.isFinite(at) && at > 0 && Date.now() - at < DISMISS_DAYS * 24 * 60 * 60 * 1000
  } catch {
    return false
  }
}

export default function InstallBanner() {
  const { t } = useI18n()
  const [, rerender] = useState(0)
  const [hidden, setHidden] = useState(() => isStandalone() || recentlyDismissed())

  useEffect(() => subscribeInstall(() => rerender((n) => n + 1)), [])

  if (hidden || wasJustInstalled()) return null

  const ios = isIos()
  // Browsers without an install prompt (e.g. Firefox desktop) get nothing.
  if (!ios && !canPromptInstall()) return null

  const dismiss = () => {
    try {
      localStorage.setItem(DISMISS_KEY, String(Date.now()))
    } catch {
      // not persisted; hidden for this session only
    }
    setHidden(true)
  }

  return (
    <aside className="install-banner card" aria-label={t('install.title')}>
      <img className="install-banner-icon" src={`${import.meta.env.BASE_URL}icons/icon-192.png`} alt="" />
      <div className="install-banner-text">
        <strong>{t('install.title')}</strong>
        <span>{ios ? t('install.iosHint') : t('install.body')}</span>
      </div>
      <div className="install-banner-actions">
        {!ios && (
          <button type="button" className="btn-primary" onClick={() => void promptInstall()}>
            {t('install.button')}
          </button>
        )}
        <button type="button" onClick={dismiss}>
          {t('install.later')}
        </button>
      </div>
    </aside>
  )
}
