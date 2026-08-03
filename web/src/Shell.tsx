import { useState } from 'react'
import { apiFetch } from './api'
import Dashboard from './Dashboard'
import Users from './Users'
import Devices from './Devices'
import AdminTest from './AdminTest'
import SystemConfig from './SystemConfig'

type Session = { username: string; role: string }
type View = 'dashboard' | 'devices' | 'users' | 'admin-test' | 'system-config'

function Shell({ session, onLoggedOut }: { session: Session; onLoggedOut: () => void }) {
  const [view, setView] = useState<View>('dashboard')

  const handleLogout = async () => {
    await apiFetch('/auth/logout', { method: 'POST' })
    onLoggedOut()
  }

  const isAdmin = session.role === 'Admin' || session.role === 'SuperAdmin'

  return (
    <div className="shell">
      <nav className="shell-nav">
        <span className="shell-user">
          {session.username} ({session.role})
        </span>
        <button type="button" onClick={() => setView('dashboard')}>
          Dashboard
        </button>
        <button type="button" onClick={() => setView('devices')}>
          Devices
        </button>
        {session.role === 'SuperAdmin' && (
          <button type="button" onClick={() => setView('users')}>
            Users
          </button>
        )}
        {isAdmin && (
          <button type="button" onClick={() => setView('admin-test')}>
            Admin test
          </button>
        )}
        {session.role === 'SuperAdmin' && (
          <button type="button" onClick={() => setView('system-config')}>
            System config
          </button>
        )}
        <button type="button" onClick={handleLogout}>
          Sign out
        </button>
      </nav>
      <div className="shell-content">
        {view === 'dashboard' && <Dashboard />}
        {view === 'devices' && <Devices isAdmin={isAdmin} />}
        {view === 'users' && session.role === 'SuperAdmin' && <Users />}
        {view === 'admin-test' && isAdmin && <AdminTest />}
        {view === 'system-config' && session.role === 'SuperAdmin' && <SystemConfig />}
      </div>
    </div>
  )
}

export default Shell
