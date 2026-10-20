import { Link } from 'react-router-dom'

export default function PageHeader() {
  return (
    <header className="page-header">
      <div className="container page-header__inner">
        <Link to="/" className="page-header__logo" aria-label="SYNORA – Trang chủ">
          <img src="/img/logo.webp" width="40" height="40" alt="" /> SYNORA
        </Link>
        <nav aria-label="Điều hướng chính">
          <Link to="/tin-tuc">Tin tức</Link>
          <Link to="/#lien-he">Liên hệ</Link>
          <Link to="/dat-hang" className="btn btn--primary">Đặt Hàng</Link>
        </nav>
      </div>
    </header>
  )
}
