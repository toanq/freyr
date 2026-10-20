import { useState } from 'react'
import { useI18n } from '../i18n'
import { register } from './accountApi'

// Mirrors app/lib/auth/auth_screen.dart's register mode (username, optional
// email, password, remember me) plus a confirm-password field.
function RegisterForm({ onRegistered }: { onRegistered: () => void }) {
  const { t } = useI18n()
  const [username, setUsername] = useState('')
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [confirm, setConfirm] = useState('')
  const [rememberMe, setRememberMe] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [submitting, setSubmitting] = useState(false)

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault()
    setError(null)
    if (!username.trim() || !password) {
      setError(t('account.required'))
      return
    }
    if (password !== confirm) {
      setError(t('account.passwordMismatch'))
      return
    }

    setSubmitting(true)
    try {
      const res = await register(username.trim(), email.trim() || null, password, rememberMe)
      if (res.ok) {
        onRegistered()
        return
      }
      setError(
        res.status === 409
          ? t('account.usernameTaken')
          : res.status === 400
            ? t('account.required')
            : res.status === 503
              ? t('account.registerNotConfigured')
              : t('account.registerFailed', { status: res.status }),
      )
    } catch (err) {
      setError(t('login.unreachable', { error: String(err) }))
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <form className="card" onSubmit={handleSubmit}>
      <label>
        {t('login.username')}
        <input value={username} onChange={(e) => setUsername(e.target.value)} autoComplete="username" required />
      </label>
      <label>
        {t('account.emailOptional')}
        <input type="email" value={email} onChange={(e) => setEmail(e.target.value)} autoComplete="email" />
      </label>
      <label>
        {t('login.password')}
        <input
          type="password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          autoComplete="new-password"
          required
        />
      </label>
      <label>
        {t('account.confirmPassword')}
        <input
          type="password"
          value={confirm}
          onChange={(e) => setConfirm(e.target.value)}
          autoComplete="new-password"
          required
        />
      </label>
      <RememberMe checked={rememberMe} onChange={setRememberMe} />
      <button type="submit" disabled={submitting}>
        {submitting ? t('account.creating') : t('account.createAccount')}
      </button>
      {error && <p className="login-error">{error}</p>}
    </form>
  )
}

export function RememberMe({ checked, onChange }: { checked: boolean; onChange: (value: boolean) => void }) {
  const { t } = useI18n()
  return (
    <label className="remember-me">
      <input type="checkbox" checked={checked} onChange={(e) => onChange(e.target.checked)} />
      {t('account.rememberMe')}
    </label>
  )
}

export default RegisterForm
