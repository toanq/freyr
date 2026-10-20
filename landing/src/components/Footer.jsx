import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { CONTACT, NAV } from '../content.js'
import { api } from '../lib/api.js'
import { FacebookIcon, MailIcon, PinIcon, TiktokIcon } from './Icons.jsx'
  const YEAR = new Date().getFullYear()

function formatDate(iso) {
  return new Date(iso).toLocaleDateString('vi-VN')
}

export default function Footer() {
  const [email, setEmail] = useState('')
  const [done, setDone] = useState(false)
  const [posts, setPosts] = useState([])

  useEffect(() => {
    let cancelled = false
    api.listPosts().then((data) => { if (!cancelled) setPosts(data.slice(0, 3)) }).catch(() => {})
    return () => { cancelled = true }
  }, [])

  return (
    <footer className="footer">
      <div className="footer__strip">
        <div className="container footer__strip-inner">
          <span><PinIcon /> {CONTACT.address}</span>
          <a href={`mailto:${CONTACT.email}`}><MailIcon /> {CONTACT.email}</a>
        </div>
      </div>

      <div className="container footer__grid">
        <div>
          <h2 className="footer__brand">Synora</h2>
          <h3>Thông tin Công ty</h3>
          <h4>GIỚI THIỆU</h4>
          <p>
            SYNORA là hệ thống chậu cây thông minh tích hợp công nghệ IoT, giúp tự động chăm sóc và theo dõi
            cây trồng qua điện thoại. Chúng tôi tự hào mang đến giải pháp tiện lợi, góp phần kiến tạo không
            gian sống xanh, hiện đại và thân thiện với môi trường.
          </p>
          <div className="footer__social">
            <a href={CONTACT.facebook} target="_blank" rel="noreferrer" aria-label="Facebook"><FacebookIcon /></a>
            <a href={CONTACT.tiktok} target="_blank" rel="noreferrer" aria-label="TikTok"><TiktokIcon /></a>
          </div>
        </div>

        <div>
          <h4>LIÊN KẾT</h4>
          <ul className="footer__links">
            {NAV.map((n) => (
              <li key={n.href}>
                {n.href.startsWith('#') ? <a href={n.href}>{n.label}</a> : <Link to={n.href}>{n.label}</Link>}
              </li>
            ))}
          </ul>
        </div>

        <div>
          <h4>BÀI VIẾT MỚI</h4>
          <ul className="footer__posts">
            {posts.map((p) => (
              <li key={p.slug}>
                <Link to={`/tin-tuc/${p.slug}`}>{p.title}</Link>
                <small>{formatDate(p.published_at ?? p.created_at)}</small>
              </li>
            ))}
          </ul>
        </div>

        <div>
          <h4>ĐĂNG KÝ NHẬN TIN</h4>
          <p>Nhận thông tin sản phẩm mới nhất, tin khuyến mãi và nhiều hơn nữa.</p>
          <form
            className="subscribe"
            onSubmit={(e) => {
              e.preventDefault()
              setDone(true)
              setEmail('')
            }}
          >
            <label htmlFor="subscribe-email" className="sr-only">Email nhận tin</label>
            <input
              id="subscribe-email"
              autoComplete="email"
              type="email"
              required
              placeholder="Email của bạn"
              value={email}
              onChange={(e) => setEmail(e.target.value)}
            />
            <button className="btn btn--primary" type="submit">Đăng ký</button>
          </form>
          <p className="form-note" role="status">{done ? 'Đăng ký thành công!' : ''}</p>
        </div>
      </div>

      <div className="footer__bottom">© {YEAR} Synora. All rights reserved.</div>
    </footer>
  )
}
