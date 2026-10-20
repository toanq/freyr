import { useState } from 'react'
import { ApiError, api } from '../../lib/api.js'

export default function AdminLogin({ onLoggedIn }) {
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)

  const submit = async (e) => {
    e.preventDefault()
    setBusy(true)
    setError('')
    try {
      await api.login(username, password)
      onLoggedIn()
    } catch (err) {
      setError(err instanceof ApiError && err.status === 401 ? 'Sai tên đăng nhập hoặc mật khẩu.' : 'Không thể đăng nhập, vui lòng thử lại.')
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="admin-login">
      <form className="admin-login__form" onSubmit={submit}>
        <img src="/img/logo.webp" width="56" height="56" alt="" />
        <h1>Đăng nhập quản trị</h1>
        <div className="field">
          <label htmlFor="admin-username">Tên đăng nhập</label>
          <input id="admin-username" required autoComplete="username" value={username} onChange={(e) => setUsername(e.target.value)} />
        </div>
        <div className="field">
          <label htmlFor="admin-password">Mật khẩu</label>
          <input
            id="admin-password"
            required
            type="password"
            autoComplete="current-password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
          />
        </div>
        <button type="submit" className="btn btn--primary" disabled={busy}>
          {busy ? 'Đang đăng nhập...' : 'Đăng nhập'}
        </button>
        {error && <p className="form-note form-note--error" role="alert">{error}</p>}
      </form>
    </div>
  )
}
