import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { api } from '../../lib/api.js'
import Breadcrumb from '../../components/admin/Breadcrumb.jsx'

export default function AdminPosts() {
  const [posts, setPosts] = useState(null)
  const [error, setError] = useState('')

  const load = () => api.listAdminPosts().then(setPosts).catch(() => setError('Không tải được danh sách bài viết.'))

  useEffect(() => {
    load()
  }, [])

  const remove = async (id) => {
    if (!window.confirm('Xoá bài viết này?')) return
    await api.deletePost(id)
    load()
  }

  return (
    <div>
      <Breadcrumb items={[{ label: 'Bài viết' }]} />
      <div className="admin__header">
        <h1>Bài viết (#tin-tuc)</h1>
        <Link to="new" className="btn btn--primary">+ Bài viết mới</Link>
      </div>
      {error && <p className="form-note form-note--error">{error}</p>}
      <table className="admin-table">
        <thead>
          <tr>
            <th>Tiêu đề</th>
            <th>Slug</th>
            <th>Danh mục</th>
            <th>Trạng thái</th>
            <th>Cập nhật</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {posts?.map((p) => (
            <tr key={p.id}>
              <td>{p.title}</td>
              <td>{p.slug}</td>
              <td>{p.category ?? '—'}</td>
              <td>{p.published ? 'Đã đăng' : 'Nháp'}</td>
              <td>{new Date(p.updated_at).toLocaleString('vi-VN')}</td>
              <td className="admin-table__actions">
                <Link to={p.id}>Sửa</Link>
                <button type="button" className="admin-table__danger" onClick={() => remove(p.id)}>
                  Xoá
                </button>
              </td>
            </tr>
          ))}
          {posts?.length === 0 && (
            <tr>
              <td colSpan={6}>Chưa có bài viết nào.</td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}
