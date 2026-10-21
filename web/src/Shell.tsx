import { useEffect, useState } from 'react'
import { apiFetch } from './api'
import { forgetPushOnSignOut, refreshPushToken } from './push/pushClient'
import Dashboard from './Dashboard'
import Users from './Users'
import Devices from './Devices'
import AdminTest from './AdminTest'
import SystemConfig from './SystemConfig'
import DuplicateDevices from './DuplicateDevices'
import MicTestUploads from './MicTestUploads'
import Firmware from './Firmware'
import { useI18n, type StringKey } from './i18n'
import { LanguageToggle } from './I18nProvider'
import AppBarAvatar from './account/AppBarAvatar'
import Profile from './account/Profile'
import Orders from './cms/Orders'
import Posts from './cms/Posts'
import InstallBanner from './install/InstallBanner'

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
  | 'orders'
  | 'posts'
  | 'profile'

const ADMIN_TABS: View[] = ['firmware', 'orders', 'posts', 'dashboard', 'admin-test', 'duplicate-devices', 'mictest-uploads']
const SUPERADMIN_TABS: View[] = ['users', 'system-config']

// ?tab=<view> keeps the current tab across reloads and shared links (e.g.
// ?tab=posts&post=<id> opens the CMS editor). Unknown or not-allowed tabs
// fall back to the device list.
function initialView(isAdmin: boolean, isSuperAdmin: boolean): View {
  const params = new URLSearchParams(window.location.search)
  if (params.has('device')) return 'devices'
  const tab = (params.get('tab') ?? (params.has('post') ? 'posts' : null)) as View | null
  if (tab && isAdmin && ADMIN_TABS.includes(tab)) return tab
  if (tab && isSuperAdmin && SUPERADMIN_TABS.includes(tab)) return tab
  return 'devices'
}

function Shell({ session, onLoggedOut }: { session: Session; onLoggedOut: () => void }) {
  const isAdmin = session.role === 'Admin' || session.role === 'SuperAdmin'
  const isSuperAdmin = session.role === 'SuperAdmin'
  const [view, setViewState] = useState<View>(() => initialView(isAdmin, isSuperAdmin))
  const { t } = useI18n()

  // Switching tabs replaces the URL (no new history entry) and drops any
  // sub-page params (?device=, ?post=) that belong to the previous tab.
  const setView = (next: View) => {
    const url = new URL(window.location.href)
    url.search = next === 'devices' || next === 'profile' ? '' : `?tab=${next}`
    window.history.replaceState(null, '', `${url.pathname}${url.search}${url.hash}`)
    setViewState(next)
  }

  useEffect(() => {
    void refreshPushToken()
  }, [])

  const handleLogout = async () => {
    // While still signed in, so the DELETE is authorized.
    await forgetPushOnSignOut()
    await apiFetch('/auth/logout', { method: 'POST' })
    onLoggedOut()
  }

  const tabs: { id: View; label: StringKey; visible: boolean }[] = [
    { id: 'devices', label: 'tab.devices', visible: true },
    { id: 'firmware', label: 'tab.firmware', visible: isAdmin },
    { id: 'orders', label: 'tab.orders', visible: isAdmin },
    { id: 'posts', label: 'tab.posts', visible: isAdmin },
    { id: 'dashboard', label: 'tab.status', visible: isAdmin },
    { id: 'users', label: 'tab.users', visible: isSuperAdmin },
    { id: 'admin-test', label: 'tab.adminTest', visible: isAdmin },
    { id: 'duplicate-devices', label: 'tab.duplicates', visible: isAdmin },
    { id: 'mictest-uploads', label: 'tab.micTests', visible: isAdmin },
    { id: 'system-config', label: 'tab.systemConfig', visible: isSuperAdmin },
  ]
  const visibleTabs = tabs.filter((tab) => tab.visible)

  return (
    <div className="shell">
      <header className="app-bar">
        <img className="app-bar-logo" src={`${import.meta.env.BASE_URL}logo-green.jpg`} alt="" />
        <span className="app-bar-title">Synora</span>
        <AppBarAvatar username={session.username} role={session.role} onOpen={() => setView('profile')} />
        <LanguageToggle />
        <button type="button" className="app-bar-signout" onClick={handleLogout}>
          {t('shell.signOut')}
        </button>
      </header>
      {visibleTabs.length > 1 && (
        <nav className="tabs" aria-label="Sections">
          {visibleTabs.map((tab) => (
            <button
              key={tab.id}
              type="button"
              className={view === tab.id ? 'tab tab--active' : 'tab'}
              aria-current={view === tab.id ? 'page' : undefined}
              onClick={() => setView(tab.id)}
            >
              {t(tab.label)}
            </button>
          ))}
        </nav>
      )}
      <main className="shell-content">
        <InstallBanner />
        {view === 'dashboard' && isAdmin && <Dashboard />}
        {view === 'firmware' && isAdmin && <Firmware />}
        {view === 'orders' && isAdmin && <Orders />}
        {view === 'posts' && isAdmin && <Posts />}
        {view === 'devices' && <Devices isAdmin={isAdmin} />}
        {view === 'users' && isSuperAdmin && <Users />}
        {view === 'admin-test' && isAdmin && <AdminTest />}
        {view === 'duplicate-devices' && isAdmin && <DuplicateDevices />}
        {view === 'mictest-uploads' && isAdmin && <MicTestUploads />}
        {view === 'system-config' && isSuperAdmin && <SystemConfig />}
        {view === 'profile' && <Profile onBack={() => setView('devices')} onSignOut={handleLogout} />}
      </main>
    </div>
  )
}

export default Shell
