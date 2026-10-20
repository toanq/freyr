import { useState } from 'react'
import { apiFetch } from './api'
import { useI18n } from './i18n'
import { LanguageToggle } from './I18nProvider'
import RegisterForm, { RememberMe } from './account/RegisterForm'
import SocialLogin from './account/SocialLogin'
import './account/account.css'

function Login({ onLoggedIn }: { onLoggedIn: () => void }) {
  const [registering, setRegistering] = useState(false)
  const [rememberMe, setRememberMe] = useState(true)
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [submitting, setSubmitting] = useState(false)
  const { t } = useI18n()

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault()
    setSubmitting(true)
    setError(null)

    try {
      const res = await apiFetch('/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password, rememberMe }),
      })

      if (res.ok) {
        onLoggedIn()
        return
      }

      if (res.status === 503) {
        const problem = await res.json()
        setError(problem.detail ?? t('login.notConfigured'))
      } else {
        setError(t('login.invalid'))
      }
    } catch (err) {
      setError(t('login.unreachable', { error: String(err) }))
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <main className="login">
      <div className="login-lang">
        <LanguageToggle />
      </div>
      <img className="login-logo" src={`${import.meta.env.BASE_URL}logo-green.jpg`} alt="" />
      <h1>Synora</h1>
      <p className="login-subtitle">{t('login.subtitle')}</p>
      {registering ? (
        <RegisterForm onRegistered={onLoggedIn} />
      ) : (
      <form className="card" onSubmit={handleSubmit}>
        <label>
          {t('login.username')}
          <input
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            autoComplete="username"
            required
          />
        </label>
        <label>
          {t('login.password')}
          <input
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoComplete="current-password"
            required
          />
        </label>
        <RememberMe checked={rememberMe} onChange={setRememberMe} />
        <button type="submit" disabled={submitting}>
          {submitting ? t('login.submitting') : t('login.submit')}
        </button>
        {error && <p className="login-error">{error}</p>}
      </form>
      )}
      <button type="button" className="login-switch" onClick={() => setRegistering(!registering)}>
        {registering ? t('account.toLogin') : t('account.toRegister')}
      </button>
      <SocialLogin onLoggedIn={onLoggedIn} />
    </main>
  )
}

export default Login
