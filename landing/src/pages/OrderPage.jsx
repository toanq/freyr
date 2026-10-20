import { useState } from 'react'
import Breadcrumb from '../components/Breadcrumb.jsx'
import Footer from '../components/Footer.jsx'
import PageHeader from '../components/PageHeader.jsx'
import { api, ApiError } from '../lib/api.js'

const GALLERY = [
  { src: '/img/order/planter-hero.webp', alt: 'Chậu cây thông minh SYNORA với cây trồng' },
  { src: '/img/order/planter-display.webp', alt: 'Màn hình SYNORA hiển thị độ ẩm đất, nhiệt độ và độ ẩm không khí' },
  { src: '/img/order/planter-speaker.webp', alt: 'Mặt bên chậu SYNORA với lưới loa và nút nguồn' },
  { src: '/img/order/planter-side.webp', alt: 'Chậu SYNORA nhìn nghiêng, cổng sạc USB-C phía dưới' },
  { src: '/img/order/planter-top.webp', alt: 'Chậu SYNORA nhìn từ trên xuống' },
  { src: '/img/order/planter-bottom.webp', alt: 'Đáy chậu SYNORA với 4 chân cảm biến và lỗ thoát nước' },
]

const EMPTY = { customer_name: '', phone: '', email: '', address: '', quantity: 1, note: '' }

export default function OrderPage() {
  const [active, setActive] = useState(0)
  const [form, setForm] = useState(EMPTY)
  const [status, setStatus] = useState('idle') // idle | sending | done | error
  const [error, setError] = useState('')

  const update = (k) => (e) => setForm({ ...form, [k]: e.target.value })
  const locked = status === 'done'

  const submit = async (e) => {
    e.preventDefault()
    setStatus('sending')
    setError('')
    try {
      await api.createOrder({
        customer_name: form.customer_name.trim(),
        phone: form.phone.trim(),
        email: form.email.trim() || null,
        address: form.address.trim() || null,
        quantity: Number(form.quantity) || 1,
        note: form.note.trim() || null,
      })
      setStatus('done')
    } catch (err) {
      setStatus('error')
      setError(err instanceof ApiError ? err.message : 'Có lỗi xảy ra, vui lòng thử lại.')
    }
  }

  return (
    <>
      <PageHeader />
      <div className="order-page">
        <Breadcrumb items={[{ label: 'Đặt hàng' }]} />

        <div className="container order-page__grid">
          <div className="order-gallery">
            <div className="order-gallery__main">
              <img src={GALLERY[active].src} alt={GALLERY[active].alt} width="800" height="800" />
            </div>
            <div className="order-gallery__thumbs">
              {GALLERY.map((g, i) => (
                <button
                  key={g.src}
                  type="button"
                  className={`order-gallery__thumb${i === active ? ' is-active' : ''}`}
                  onClick={() => setActive(i)}
                  aria-label={g.alt}
                  aria-pressed={i === active}
                >
                  <img src={g.src} alt="" width="120" height="120" />
                </button>
              ))}
            </div>
          </div>

          <form className="order-form" onSubmit={submit}>
            <p className="eyebrow">ĐẶT HÀNG</p>
            <h1 className="section__title">SYNORA Smart Planter</h1>
            <p>Để lại thông tin, đội ngũ SYNORA sẽ liên hệ xác nhận đơn hàng và tư vấn giao nhận trong 24 giờ.</p>

            <div className="form-row">
              <div className="field">
                <label htmlFor="order-name" className="sr-only">Họ và tên *</label>
                <input
                  id="order-name"
                  required
                  autoComplete="name"
                  placeholder="Họ và tên *"
                  readOnly={locked}
                  value={form.customer_name}
                  onChange={update('customer_name')}
                />
              </div>
              <div className="field">
                <label htmlFor="order-phone" className="sr-only">Số điện thoại *</label>
                <input
                  id="order-phone"
                  required
                  type="tel"
                  inputMode="tel"
                  autoComplete="tel"
                  pattern="[0-9+ ]{8,15}"
                  placeholder="Số điện thoại *"
                  aria-describedby="order-phone-hint"
                  readOnly={locked}
                  value={form.phone}
                  onChange={update('phone')}
                />
                <small id="order-phone-hint">Ví dụ: 0901 234 567</small>
              </div>
            </div>

            <div className="form-row">
              <div className="field">
                <label htmlFor="order-email" className="sr-only">Email</label>
                <input
                  id="order-email"
                  type="email"
                  autoComplete="email"
                  placeholder="Email"
                  readOnly={locked}
                  value={form.email}
                  onChange={update('email')}
                />
              </div>
              <div className="field">
                <label htmlFor="order-qty" className="sr-only">Số lượng</label>
                <input
                  id="order-qty"
                  type="number"
                  min="1"
                  max="99"
                  placeholder="Số lượng"
                  readOnly={locked}
                  value={form.quantity}
                  onChange={update('quantity')}
                />
              </div>
            </div>

            <div className="field">
              <label htmlFor="order-address" className="sr-only">Địa chỉ giao hàng</label>
              <input
                id="order-address"
                autoComplete="street-address"
                placeholder="Địa chỉ giao hàng"
                readOnly={locked}
                value={form.address}
                onChange={update('address')}
              />
            </div>

            <div className="field">
              <label htmlFor="order-note" className="sr-only">Ghi chú</label>
              <textarea
                id="order-note"
                rows={4}
                maxLength={2000}
                placeholder="Ghi chú"
                readOnly={locked}
                value={form.note}
                onChange={update('note')}
              />
            </div>

            <div className="form-actions">
              <button type="submit" className="btn btn--primary" disabled={status === 'sending' || locked}>
                {status === 'sending' ? 'Đang gửi...' : locked ? 'Đã gửi' : 'Gửi đơn đặt hàng'}
              </button>
            </div>
            <p className="form-note" role="status">
              {status === 'done' && 'Cảm ơn bạn! Đơn đặt hàng đã được ghi nhận, SYNORA sẽ liên hệ sớm nhất.'}
              {status === 'error' && error}
            </p>
          </form>
        </div>
      </div>
      <Footer />
    </>
  )
}
