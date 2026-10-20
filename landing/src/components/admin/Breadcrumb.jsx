import { Link } from 'react-router-dom'

export default function Breadcrumb({ items }) {
  return (
    <nav className="breadcrumb" aria-label="breadcrumb">
      <Link to="/admin/posts">Admin</Link>
      {items.map((item, i) => (
        <span key={i}>
          <span className="breadcrumb__sep">/</span>
          {item.to ? <Link to={item.to}>{item.label}</Link> : <span className="breadcrumb__current">{item.label}</span>}
        </span>
      ))}
    </nav>
  )
}
