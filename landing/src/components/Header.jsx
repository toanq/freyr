import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { NAV } from '../content.js'
import { CloseIcon, MenuIcon } from './Icons.jsx'

export default function Header({ onOrder }) {
  const [scrolled, setScrolled] = useState(false)
  const [menuOpen, setMenuOpen] = useState(false)

  useEffect(() => {
    const onScroll = () => setScrolled(window.scrollY > 40)
    onScroll()
    window.addEventListener('scroll', onScroll, { passive: true })
    return () => window.removeEventListener('scroll', onScroll)
  }, [])

  return (
    <header className={`header${scrolled || menuOpen ? ' is-scrolled' : ''}`}>
      <a href="#main" className="skip-link">Bỏ qua đến nội dung chính</a>
      <div className="container header__inner">
        <a href="#main" className="header__logo" aria-label="SYNORA – Trang chủ">
          <img src="/img/logo.webp" width="512" height="512" alt="" />
        </a>
        <nav id="main-nav" aria-label="Điều hướng chính" className={`header__nav${menuOpen ? ' is-open' : ''}`}>
          {NAV.map((n) =>
            n.href.startsWith('#') ? (
              <a key={n.href} href={n.href} onClick={() => setMenuOpen(false)}>
                {n.label}
              </a>
            ) : (
              <Link key={n.href} to={n.href} onClick={() => setMenuOpen(false)}>
                {n.label}
              </Link>
            ),
          )}
        </nav>
        <div className="header__actions">
          <button type="button" className="btn btn--primary" onClick={onOrder}>
            Đặt Hàng
          </button>
          <button
            type="button"
            className="header__toggle"
            aria-label={menuOpen ? 'Đóng menu' : 'Mở menu'}
            aria-controls="main-nav"
            aria-expanded={menuOpen}
            onClick={() => setMenuOpen((v) => !v)}
          >
            {menuOpen ? <CloseIcon /> : <MenuIcon />}
          </button>
        </div>
      </div>
    </header>
  )
}
