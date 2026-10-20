import { NavLink, Outlet } from 'react-router-dom'
import { api } from '../../lib/api.js'

export default function AdminLayout({ onLoggedOut }) {
  const logout = async () => {
    await api.logout().catch(() => {})
    onLoggedOut()
  }

  return (
    <div className="admin">
      <aside className="admin__sidebar">
        <div className="admin__brand">
          <img src="/img/logo.webp" width="32" height="32" alt="" /> SYNORA CMS
        </div>
        <nav>
          <NavLink to="/admin/posts" className={({ isActive }) => (isActive ? 'is-active' : undefined)}>
            Bài viết
          </NavLink>
          <NavLink to="/admin/orders" className={({ isActive }) => (isActive ? 'is-active' : undefined)}>
            Đơn hàng
          </NavLink>
        </nav>
        <button type="button" className="btn btn--ghost admin__logout" onClick={logout}>
          Đăng xuất
        </button>
      </aside>
      <main className="admin__content">
        <Outlet />
      </main>
    </div>
  )
}
