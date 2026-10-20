import { useEffect, useState } from 'react'
import { Navigate, Route, Routes } from 'react-router-dom'
import { api } from '../../lib/api.js'
import AdminLayout from './AdminLayout.jsx'
import AdminLogin from './AdminLogin.jsx'
import AdminOrders from './AdminOrders.jsx'
import AdminPostEditor from './AdminPostEditor.jsx'
import AdminPosts from './AdminPosts.jsx'

export default function AdminApp() {
  const [status, setStatus] = useState('checking') // checking | out | in

  const checkSession = () => api.me().then(() => setStatus('in')).catch(() => setStatus('out'))

  useEffect(() => {
    checkSession()
  }, [])

  if (status === 'checking') return <div className="admin-loading">Đang kiểm tra phiên đăng nhập...</div>

  if (status === 'out') {
    return (
      <Routes>
        <Route path="*" element={<AdminLogin onLoggedIn={() => setStatus('in')} />} />
      </Routes>
    )
  }

  return (
    <Routes>
      <Route element={<AdminLayout onLoggedOut={() => setStatus('out')} />}>
        <Route index element={<Navigate to="posts" replace />} />
        <Route path="posts" element={<AdminPosts />} />
        <Route path="posts/new" element={<AdminPostEditor />} />
        <Route path="posts/:id" element={<AdminPostEditor />} />
        <Route path="orders" element={<AdminOrders />} />
      </Route>
    </Routes>
  )
}
