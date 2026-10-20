import { useEffect, useState } from 'react'
import { useI18n } from '../i18n'
import { avatarSrc, fetchProfile, PROFILE_CHANGED_EVENT } from './accountApi'
import './account.css'

// App-bar user chip; opens the account page like the app's AppBar avatar.
function AppBarAvatar({ username, role, onOpen }: { username: string; role: string; onOpen: () => void }) {
  const { t } = useI18n()
  const [avatar, setAvatar] = useState<{ url: string; version: number } | null>(null)

  useEffect(() => {
    const load = () =>
      fetchProfile()
        .then((p) => setAvatar(p.avatarUrl ? { url: p.avatarUrl, version: Date.now() } : null))
        .catch(() => {})
    load()
    window.addEventListener(PROFILE_CHANGED_EVENT, load)
    return () => window.removeEventListener(PROFILE_CHANGED_EVENT, load)
  }, [])

  return (
    <button type="button" className="app-bar-user app-bar-user-button" title={role} onClick={onOpen} aria-label={t('account.openProfile')}>
      <span className="app-bar-avatar" aria-hidden="true">
        {avatar ? <img src={avatarSrc(avatar.url, avatar.version)} alt="" /> : username.charAt(0).toUpperCase()}
      </span>
      <span className="app-bar-username">{username}</span>
    </button>
  )
}

export default AppBarAvatar
