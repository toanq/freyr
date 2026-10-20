import { useEffect, useState } from 'react'
import { api } from '../../lib/api.js'
import Breadcrumb from '../../components/admin/Breadcrumb.jsx'

const STATUSES = ['new', 'confirmed', 'shipped', 'done', 'cancelled']
const STATUS_LABEL = { new: 'Mới', confirmed: 'Đã xác nhận', shipped: 'Đang giao', done: 'Hoàn tất', cancelled: 'Đã huỷ' }

export default function AdminOrders() {
  const [orders, setOrders] = useState(null)
  const [error, setError] = useState('')
  const [filter, setFilter] = useState('all')

  const load = () => api.listAdminOrders().then(setOrders).catch(() => setError('Không tải được danh sách đơn hàng.'))

  useEffect(() => {
    load()
  }, [])

  const changeStatus = async (id, status) => {
    await api.updateOrderStatus(id, status)
    load()
  }

  const toggleProcessed = (o) => changeStatus(o.id, o.status === 'done' ? 'new' : 'done')

  const visible = orders?.filter((o) => filter === 'all' || o.status === filter)

  return (
    <div>
      <Breadcrumb items={[{ label: 'Đơn hàng' }]} />
      <div className="admin__header">
        <h1>Đơn hàng</h1>
        <select value={filter} onChange={(e) => setFilter(e.target.value)}>
          <option value="all">Tất cả trạng thái</option>
          {STATUSES.map((s) => (
            <option key={s} value={s}>
              {STATUS_LABEL[s]}
            </option>
          ))}
        </select>
      </div>
      {error && <p className="form-note form-note--error">{error}</p>}
      <table className="admin-table">
        <thead>
          <tr>
            <th>Khách hàng</th>
            <th>Điện thoại</th>
            <th>Email</th>
            <th>Địa chỉ</th>
            <th>SL</th>
            <th>Ghi chú</th>
            <th>Đã xử lý</th>
            <th>Trạng thái</th>
            <th>Thời gian</th>
          </tr>
        </thead>
        <tbody>
          {visible?.map((o) => (
            <tr key={o.id}>
              <td>{o.customer_name}</td>
              <td><a href={`tel:${o.phone}`}>{o.phone}</a></td>
              <td>{o.email ? <a href={`mailto:${o.email}`}>{o.email}</a> : '—'}</td>
              <td>{o.address ?? '—'}</td>
              <td>{o.quantity}</td>
              <td>{o.note ?? '—'}</td>
              <td>
                <input type="checkbox" checked={o.status === 'done'} onChange={() => toggleProcessed(o)} />
              </td>
              <td>
                <select value={o.status} onChange={(e) => changeStatus(o.id, e.target.value)}>
                  {STATUSES.map((s) => (
                    <option key={s} value={s}>
                      {STATUS_LABEL[s]}
                    </option>
                  ))}
                </select>
              </td>
              <td>{new Date(o.created_at).toLocaleString('vi-VN')}</td>
            </tr>
          ))}
          {visible?.length === 0 && (
            <tr>
              <td colSpan={9}>Chưa có đơn hàng nào.</td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}
