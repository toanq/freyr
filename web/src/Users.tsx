import { useEffect, useState } from 'react'
import { apiFetch } from './api'

type UserRow = {
  id: string
  username: string
  email: string | null
  role: string
  createdAt: string
  hasPushToken: boolean
}

function Users() {
  const [users, setUsers] = useState<UserRow[]>([])
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [role, setRole] = useState<'User' | 'Admin'>('User')
  const [error, setError] = useState<string | null>(null)

  const [notifyingUserId, setNotifyingUserId] = useState<string | null>(null)
  const [notifyTitle, setNotifyTitle] = useState('')
  const [notifyBody, setNotifyBody] = useState('')
  const [notifyStatus, setNotifyStatus] = useState<string | null>(null)

  const [resettingUserId, setResettingUserId] = useState<string | null>(null)
  const [newPassword, setNewPassword] = useState('')
  const [resetStatus, setResetStatus] = useState<string | null>(null)

  const load = () => {
    apiFetch('/users')
      .then((res) => res.json())
      .then(setUsers)
      .catch(() => setError('Could not load users.'))
  }

  useEffect(load, [])

  const handleCreate = async (e: React.FormEvent) => {
    e.preventDefault()
    setError(null)

    const res = await apiFetch('/users', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, password, role }),
    })

    if (!res.ok) {
      setError(res.status === 409 ? 'Username already exists.' : `Could not create user (HTTP ${res.status}).`)
      return
    }

    setUsername('')
    setPassword('')
    setRole('User')
    load()
  }

  const openNotifyForm = (userId: string) => {
    setNotifyingUserId(userId)
    setNotifyTitle('')
    setNotifyBody('')
    setNotifyStatus(null)
  }

  const handleSendNotification = async (e: React.FormEvent, userId: string) => {
    e.preventDefault()
    setNotifyStatus('Sending…')

    const res = await apiFetch(`/users/${userId}/notifications/test`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ title: notifyTitle, body: notifyBody }),
    })

    if (res.ok) {
      setNotifyStatus('Sent.')
    } else if (res.status === 503) {
      setNotifyStatus('Push notifications are not configured on the server.')
    } else if (res.status === 400) {
      setNotifyStatus('This user has no registered device.')
    } else {
      setNotifyStatus(`Failed to send (HTTP ${res.status}).`)
    }
  }

  const openResetForm = (userId: string) => {
    setResettingUserId(userId)
    setNewPassword('')
    setResetStatus(null)
  }

  const handleResetPassword = async (e: React.FormEvent, userId: string) => {
    e.preventDefault()
    setResetStatus('Saving…')

    const res = await apiFetch(`/users/${userId}/reset-password`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ newPassword }),
    })

    if (res.ok) {
      setResetStatus('Password reset.')
    } else if (res.status === 400) {
      setResetStatus('Password must be at least 6 characters.')
    } else {
      setResetStatus(`Failed (HTTP ${res.status}).`)
    }
  }

  return (
    <section className="users">
      <h2>Users</h2>
      <form className="inline-form card" onSubmit={handleCreate}>
        <input placeholder="Username" value={username} onChange={(e) => setUsername(e.target.value)} required />
        <input
          type="password"
          placeholder="Password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          required
        />
        <select value={role} onChange={(e) => setRole(e.target.value as 'User' | 'Admin')}>
          <option value="User">User</option>
          <option value="Admin">Admin</option>
        </select>
        <button type="submit">Create</button>
      </form>
      {error && <p className="form-error">{error}</p>}
      <div className="table-scroll card">
      <table>
        <thead>
          <tr>
            <th>Username</th>
            <th>Email</th>
            <th>Role</th>
            <th>Created</th>
            <th>Push</th>
            <th>Password</th>
          </tr>
        </thead>
        <tbody>
          {users.map((u) => (
            <>
              <tr key={u.id}>
                <td>{u.username}</td>
                <td>{u.email ?? '—'}</td>
                <td>{u.role}</td>
                <td>{new Date(u.createdAt).toLocaleString()}</td>
                <td>
                  {u.hasPushToken ? (
                    <button type="button" onClick={() => openNotifyForm(u.id)}>
                      Send test notification
                    </button>
                  ) : (
                    <span title="This user has no device registered for push notifications.">No device</span>
                  )}
                </td>
                <td>
                  <button type="button" onClick={() => openResetForm(u.id)}>
                    Reset password
                  </button>
                </td>
              </tr>
              {resettingUserId === u.id && (
                <tr key={`${u.id}-reset`}>
                  <td colSpan={6}>
                    <form className="inline-form" onSubmit={(e) => handleResetPassword(e, u.id)}>
                      <input
                        type="password"
                        placeholder="New password"
                        value={newPassword}
                        onChange={(e) => setNewPassword(e.target.value)}
                        minLength={6}
                        required
                      />
                      <button type="submit">Save</button>
                      <button type="button" onClick={() => setResettingUserId(null)}>
                        Cancel
                      </button>
                      {resetStatus && <span> {resetStatus}</span>}
                    </form>
                  </td>
                </tr>
              )}
              {notifyingUserId === u.id && (
                <tr key={`${u.id}-notify`}>
                  <td colSpan={6}>
                    <form className="inline-form" onSubmit={(e) => handleSendNotification(e, u.id)}>
                      <input
                        placeholder="Title"
                        value={notifyTitle}
                        onChange={(e) => setNotifyTitle(e.target.value)}
                        required
                      />
                      <input
                        placeholder="Message"
                        value={notifyBody}
                        onChange={(e) => setNotifyBody(e.target.value)}
                        required
                      />
                      <button type="submit">Send</button>
                      <button type="button" onClick={() => setNotifyingUserId(null)}>
                        Cancel
                      </button>
                      {notifyStatus && <span> {notifyStatus}</span>}
                    </form>
                  </td>
                </tr>
              )}
            </>
          ))}
        </tbody>
      </table>
      </div>
    </section>
  )
}

export default Users
