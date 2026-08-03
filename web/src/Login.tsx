import { useState } from 'react'
import { apiFetch } from './api'

function Login({ onLoggedIn }: { onLoggedIn: () => void }) {
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [error, setError] = useState<string | null>(null)
  const [submitting, setSubmitting] = useState(false)

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault()
    setSubmitting(true)
    setError(null)

    try {
      const res = await apiFetch('/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password }),
      })

      if (res.ok) {
        onLoggedIn()
        return
      }

      if (res.status === 503) {
        const problem = await res.json()
        setError(problem.detail ?? 'Login is not configured on the server.')
      } else {
        setError('Invalid username or password.')
      }
    } catch (err) {
      setError(`Could not reach the server (${String(err)})`)
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <main className="login">
      <h1>Synora</h1>
      <form onSubmit={handleSubmit}>
        <label>
          Username
          <input
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            autoComplete="username"
            required
          />
        </label>
        <label>
          Password
          <input
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoComplete="current-password"
            required
          />
        </label>
        <button type="submit" disabled={submitting}>
          {submitting ? 'Signing in…' : 'Sign in'}
        </button>
        {error && <p className="login-error">{error}</p>}
      </form>
    </main>
  )
}

export default Login
