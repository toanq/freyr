import { useEffect, useRef, useState } from 'react'
import { useI18n } from '../i18n'
import {
  avatarSrc,
  changePassword,
  fetchProfile,
  notifyProfileChanged,
  updateEmail,
  uploadAvatar,
  type Profile as ProfileData,
} from './accountApi'
import './account.css'
import PushCard from '../push/PushCard'

const AVATAR_TYPES = ['image/jpeg', 'image/png', 'image/webp']
const AVATAR_MAX_BYTES = 8 * 1024 * 1024

type Notice = { kind: 'ok' | 'error'; text: string } | null

// Port of app/lib/auth/user_profile_screen.dart: avatar, email, password, sign out.
function Profile({ onBack, onSignOut }: { onBack: () => void; onSignOut: () => void }) {
  const { t } = useI18n()
  const [profile, setProfile] = useState<ProfileData | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [avatarVersion, setAvatarVersion] = useState<number>()
  const [uploading, setUploading] = useState(false)
  const [notice, setNotice] = useState<Notice>(null)
  const fileInput = useRef<HTMLInputElement>(null)

  useEffect(() => {
    fetchProfile()
      .then(setProfile)
      .catch((err) => setLoadError(String(err)))
  }, [])

  const handleAvatar = async (file: File | undefined) => {
    if (!file) return
    if (!AVATAR_TYPES.includes(file.type) || file.size > AVATAR_MAX_BYTES) {
      setNotice({ kind: 'error', text: t('account.avatarType') })
      return
    }
    setUploading(true)
    setNotice(null)
    try {
      const avatarUrl = await uploadAvatar(file)
      setProfile((p) => (p ? { ...p, avatarUrl } : p))
      setAvatarVersion(Date.now())
      notifyProfileChanged()
    } catch (err) {
      setNotice({ kind: 'error', text: t('account.avatarFailed', { error: String(err) }) })
    } finally {
      setUploading(false)
      if (fileInput.current) fileInput.current.value = ''
    }
  }

  if (loadError) return <p className="form-error">{t('account.loadFailed', { error: loadError })}</p>
  if (!profile) return <p className="form-hint">{t('discussions.loading')}</p>

  return (
    <section className="profile">
      <div className="page-header">
        <button type="button" className="profile-back" onClick={onBack}>
          {t('account.back')}
        </button>
        <h2>{t('account.title')}</h2>
      </div>

      <div className="card profile-hero">
        <button
          type="button"
          className="profile-avatar"
          onClick={() => fileInput.current?.click()}
          disabled={uploading}
          aria-label={t('account.changeAvatar')}
        >
          {profile.avatarUrl ? (
            <img src={avatarSrc(profile.avatarUrl, avatarVersion)} alt="" />
          ) : (
            <span aria-hidden="true">{profile.username.charAt(0).toUpperCase()}</span>
          )}
        </button>
        <input
          ref={fileInput}
          type="file"
          accept={AVATAR_TYPES.join(',')}
          hidden
          onChange={(e) => handleAvatar(e.target.files?.[0])}
        />
        <strong className="profile-name">{profile.username}</strong>
        <button type="button" onClick={() => fileInput.current?.click()} disabled={uploading}>
          {uploading ? t('account.uploading') : t('account.changeAvatar')}
        </button>
      </div>

      {notice && <p className={notice.kind === 'ok' ? 'form-status' : 'form-error'}>{notice.text}</p>}

      <EmailCard
        email={profile.email}
        onSaved={(email) => {
          setProfile({ ...profile, email })
          setNotice({ kind: 'ok', text: t('account.emailSaved') })
        }}
        onError={(text) => setNotice({ kind: 'error', text })}
      />
      <PasswordCard onDone={(n) => setNotice(n)} />
      <PushCard />

      <button type="button" className="btn-danger profile-signout" onClick={onSignOut}>
        {t('shell.signOut')}
      </button>
    </section>
  )
}

function EmailCard({
  email,
  onSaved,
  onError,
}: {
  email: string | null
  onSaved: (email: string | null) => void
  onError: (text: string) => void
}) {
  const { t } = useI18n()
  const [editing, setEditing] = useState(false)
  const [value, setValue] = useState(email ?? '')
  const [saving, setSaving] = useState(false)

  const save = async (e: React.FormEvent) => {
    e.preventDefault()
    setSaving(true)
    const next = value.trim() || null
    try {
      const res = await updateEmail(next)
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      onSaved(next)
      setEditing(false)
    } catch (err) {
      onError(t('account.emailFailed', { error: String(err) }))
    } finally {
      setSaving(false)
    }
  }

  if (!editing) {
    return (
      <div className="card profile-row">
        <div>
          <span className="profile-label">{t('account.email')}</span>
          <span>{email ?? t('account.notSet')}</span>
        </div>
        <button
          type="button"
          onClick={() => {
            setValue(email ?? '')
            setEditing(true)
          }}
        >
          {t('account.editEmail')}
        </button>
      </div>
    )
  }

  return (
    <form className="card profile-form" onSubmit={save}>
      <label>
        {t('account.email')}
        <input type="email" value={value} onChange={(e) => setValue(e.target.value)} autoComplete="email" autoFocus />
      </label>
      <div className="profile-actions">
        <button type="button" onClick={() => setEditing(false)}>
          {t('account.cancel')}
        </button>
        <button type="submit" disabled={saving}>
          {t('account.save')}
        </button>
      </div>
    </form>
  )
}

function PasswordCard({ onDone }: { onDone: (notice: Notice) => void }) {
  const { t } = useI18n()
  const [open, setOpen] = useState(false)
  const [current, setCurrent] = useState('')
  const [next, setNext] = useState('')
  const [confirm, setConfirm] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)

  const reset = () => {
    setOpen(false)
    setCurrent('')
    setNext('')
    setConfirm('')
    setError(null)
  }

  const save = async (e: React.FormEvent) => {
    e.preventDefault()
    if (next.length < 6) return setError(t('account.passwordTooShort'))
    if (next !== confirm) return setError(t('account.passwordMismatch'))
    setSaving(true)
    setError(null)
    try {
      const res = await changePassword(current, next)
      if (res.ok) {
        reset()
        onDone({ kind: 'ok', text: t('account.passwordChanged') })
      } else if (res.status === 400) {
        // The only 400 left after the client-side checks is a wrong current password.
        setError(t('account.wrongCurrentPassword'))
      } else {
        setError(t('account.passwordFailed', { error: `HTTP ${res.status}` }))
      }
    } catch (err) {
      setError(t('account.passwordFailed', { error: String(err) }))
    } finally {
      setSaving(false)
    }
  }

  if (!open) {
    return (
      <div className="card profile-row">
        <span>{t('account.changePassword')}</span>
        <button type="button" onClick={() => setOpen(true)}>
          {t('account.changePassword')}
        </button>
      </div>
    )
  }

  return (
    <form className="card profile-form" onSubmit={save}>
      <h3>{t('account.changePassword')}</h3>
      <label>
        {t('account.currentPassword')}
        <input
          type="password"
          value={current}
          onChange={(e) => setCurrent(e.target.value)}
          autoComplete="current-password"
          required
        />
      </label>
      <label>
        {t('account.newPassword')}
        <input type="password" value={next} onChange={(e) => setNext(e.target.value)} autoComplete="new-password" required />
      </label>
      <label>
        {t('account.confirmNewPassword')}
        <input
          type="password"
          value={confirm}
          onChange={(e) => setConfirm(e.target.value)}
          autoComplete="new-password"
          required
        />
      </label>
      <p className="form-hint">{t('account.noPasswordHint')}</p>
      {error && <p className="form-error">{error}</p>}
      <div className="profile-actions">
        <button type="button" onClick={reset}>
          {t('account.cancel')}
        </button>
        <button type="submit" disabled={saving}>
          {t('account.save')}
        </button>
      </div>
    </form>
  )
}

export default Profile
