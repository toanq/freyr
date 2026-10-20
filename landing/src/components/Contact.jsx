import { cloneElement, useState } from 'react'
import { CONTACT } from '../content.js'
import { MailIcon, PinIcon } from './Icons.jsx'

const EMPTY = { name: '', phone: '', email: '', address: '', message: '' }
const LABELS = { name: 'Họ và tên', phone: 'Số điện thoại', email: 'Email', address: 'Địa chỉ', message: 'Nội dung' }

export default function Contact() {
  const [form, setForm] = useState(EMPTY)
  const [sent, setSent] = useState(false)

  const update = (k) => (e) => setForm({ ...form, [k]: e.target.value })

  const submit = (e) => {
    e.preventDefault()
    // No backend yet: hand the request off to the visitor's mail client.
    const body = Object.entries(form)
      .filter(([, v]) => v.trim())
      .map(([k, v]) => `${LABELS[k]}: ${v.trim()}`)
      .join('\n')
    window.location.href = `mailto:${CONTACT.email}?subject=${encodeURIComponent(
      'Yêu cầu tư vấn / báo giá',
    )}&body=${encodeURIComponent(body)}`
    setSent(true)
  }

  return (
    <section id="lien-he" className="contact section" aria-labelledby="contact-title">
      <div className="container contact__grid">
        <form className="contact__form reveal" onSubmit={submit} noValidate={false}>
          <p className="eyebrow">LIÊN HỆ TƯ VẤN</p>
          <h2 id="contact-title" className="section__title">Gửi yêu cầu tư vấn hoặc báo giá</h2>
          <div className="form-row">
            <Field id="name" label="Họ và tên" required>
              <input id="name" required autoComplete="name" value={form.name} onChange={update('name')} />
            </Field>
            <Field id="phone" label="Số điện thoại" required hint="Ví dụ: 0901 234 567">
              <input
                id="phone"
                required
                type="tel"
                inputMode="tel"
                autoComplete="tel"
                pattern="[0-9+ ]{8,15}"
                aria-describedby="phone-hint"
                value={form.phone}
                onChange={update('phone')}
              />
            </Field>
          </div>
          <div className="form-row">
            <Field id="email" label="Email">
              <input id="email" type="email" autoComplete="email" value={form.email} onChange={update('email')} />
            </Field>
            <Field id="address" label="Địa chỉ">
              <input id="address" autoComplete="street-address" value={form.address} onChange={update('address')} />
            </Field>
          </div>
          <Field id="message" label="Nội dung" required>
            <textarea
              id="message"
              required
              rows={5}
              maxLength={2000}
              placeholder="Bạn cần tư vấn về sản phẩm, số lượng hay báo giá?"
              value={form.message}
              onChange={update('message')}
            />
          </Field>
          <div className="form-actions">
            <button type="submit" className="btn btn--primary">Gửi yêu cầu</button>
            <button
              type="button"
              className="btn btn--ghost"
              onClick={() => {
                setForm(EMPTY)
                setSent(false)
              }}
            >
              Nhập lại
            </button>
          </div>
          <p className="form-note" role="status">
            {sent ? 'Ứng dụng email của bạn đã được mở — hãy bấm gửi để hoàn tất.' : ''}
          </p>
        </form>

        <div className="contact__info">
          <InfoCard icon={<PinIcon />} label="Địa chỉ">{CONTACT.address}</InfoCard>
          <InfoCard icon={<MailIcon />} label="Email">
            <a href={`mailto:${CONTACT.email}`}>{CONTACT.email}</a>
          </InfoCard>
        </div>
      </div>
    </section>
  )
}

function Field({ id, label, required, hint, children }) {
  const placeholder = `${label}${required ? ' *' : ''}`
  return (
    <div className="field">
      <label htmlFor={id} className="sr-only">
        {label}
        {required && <span aria-hidden="true"> *</span>}
      </label>
      {cloneElement(children, { placeholder: children.props.placeholder ?? placeholder })}
      {hint && <small id={`${id}-hint`}>{hint}</small>}
    </div>
  )
}

function InfoCard({ icon, label, children }) {
  return (
    <div className="info-card reveal">
      <span className="info-card__icon">{icon}</span>
      <div>
        <h3>{label}</h3>
        <p>{children}</p>
      </div>
    </div>
  )
}
