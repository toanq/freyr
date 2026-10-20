import { useState } from 'react'
import { apiFetch } from './api'
import Dashboard from './Dashboard'
import Users from './Users'
import Devices from './Devices'
import AdminTest from './AdminTest'
import SystemConfig from './SystemConfig'
import DuplicateDevices from './DuplicateDevices'
import MicTestUploads from './MicTestUploads'
import Firmware from './Firmware'

type Session = { username: string; role: string }
type View =
  | 'dashboard'
  | 'devices'
  | 'firmware'
  | 'users'
  | 'admin-test'
  | 'system-config'
  | 'duplicate-devices'
  | 'mictest-uploads'

function Shell({ session, onLoggedOut }: { session: Session; onLoggedOut: () => void }) {
  const [view, setView] = useState<View>('devices')

  const handleLogout = async () => {
    await apiFetch('/auth/logout', { method: 'POST' })
    onLoggedOut()
  }

  const isAdmin = session.role === 'Admin' || session.role === 'SuperAdmin'
  const isSuperAdmin = session.role === 'SuperAdmin'

  const tabs: { id: View; label: string; visible: boolean }[] = [
    { id: 'devices', label: 'Devices', visible: true },
    { id: 'firmware', label: 'Firmware', visible: isAdmin },
    { id: 'dashboard', label: 'Status', visible: isAdmin },
    { id: 'users', label: 'Users', visible: isSuperAdmin },
    { id: 'admin-test', label: 'Admin test', visible: isAdmin },
    { id: 'duplicate-devices', label: 'Duplicates', visible: isAdmin },
    { id: 'mictest-uploads', label: 'Mic tests', visible: isAdmin },
    { id: 'system-config', label: 'System config', visible: isSuperAdmin },
  ]
  const visibleTabs = tabs.filter((t) => t.visible)

  return (
    <div className="shell">
      <header className="app-bar">
        <img className="app-bar-logo" src="/logo-green.jpg" alt="" />
        <span className="app-bar-title">Synora</span>
        <span className="app-bar-user" title={session.role}>
          <span className="app-bar-avatar" aria-hidden="true">
            {session.username.charAt(0).toUpperCase()}
          </span>
          <span className="app-bar-username">{session.username}</span>
        </span>
        <button type="button" className="app-bar-signout" onClick={handleLogout}>
          Sign out
        </button>
      </header>
      {visibleTabs.length > 1 && (
        <nav className="tabs" aria-label="Sections">
          {visibleTabs.map((t) => (
            <button
              key={t.id}
              type="button"
              className={view === t.id ? 'tab tab--active' : 'tab'}
              aria-current={view === t.id ? 'page' : undefined}
              onClick={() => setView(t.id)}
            >
              {t.label}
            </button>
          ))}
        </nav>
      )}
      <main className="shell-content">
        {view === 'dashboard' && isAdmin && <Dashboard />}
        {view === 'firmware' && isAdmin && <Firmware />}
        {view === 'devices' && <Devices isAdmin={isAdmin} />}
        {view === 'users' && isSuperAdmin && <Users />}
        {view === 'admin-test' && isAdmin && <AdminTest />}
        {view === 'duplicate-devices' && isAdmin && <DuplicateDevices />}
        {view === 'mictest-uploads' && isAdmin && <MicTestUploads />}
        {view === 'system-config' && isSuperAdmin && <SystemConfig />}
      </main>
    </div>
  )
}

export default Shell
