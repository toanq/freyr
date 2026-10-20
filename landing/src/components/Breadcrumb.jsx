import { Link } from 'react-router-dom'

export default function Breadcrumb({ items }) {
  return (
    <nav className="breadcrumb breadcrumb--public container" aria-label="breadcrumb">
      <Link to="/">Trang chủ</Link>
      {items.map((item, i) => (
        <span key={i}>
          <span className="breadcrumb__sep">/</span>
          {item.to ? <Link to={item.to}>{item.label}</Link> : <span className="breadcrumb__current">{item.label}</span>}
        </span>
      ))}
    </nav>
  )
}
