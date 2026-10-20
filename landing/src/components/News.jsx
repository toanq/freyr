import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { api } from '../lib/api.js'
import { CalendarIcon } from './Icons.jsx'

function formatDate(iso) {
  return new Date(iso).toLocaleDateString('vi-VN')
}

export default function News() {
  const [posts, setPosts] = useState(null)
  const [category, setCategory] = useState('all')

  useEffect(() => {
    let cancelled = false
    api
      .listPosts()
      .then((data) => {
        if (!cancelled) setPosts(data)
      })
      .catch(() => {
        if (!cancelled) setPosts([])
      })
    return () => {
      cancelled = true
    }
  }, [])

  const categories = posts ? [...new Set(posts.map((p) => p.category).filter(Boolean))] : []

  const items = (posts ?? [])
    .filter((p) => category === 'all' || p.category === category)
    .map((p) => ({ slug: p.slug, title: p.title, date: formatDate(p.published_at ?? p.created_at) }))

  return (
    <section id="tin-tuc" className="news section">
      <div className="container">
        <p className="eyebrow eyebrow--center reveal">TIN TỨC</p>
        <h2 className="section__title section__title--center reveal">Tin tức cập nhật</h2>
        {categories.length > 0 && (
          <div className="news__filters reveal">
            <button type="button" className={category === 'all' ? 'is-active' : ''} onClick={() => setCategory('all')}>
              Tất cả
            </button>
            {categories.map((c) => (
              <button key={c} type="button" className={category === c ? 'is-active' : ''} onClick={() => setCategory(c)}>
                {c}
              </button>
            ))}
          </div>
        )}
        <div className="news__grid">
          {items.map((p) => (
            <Link key={p.slug} to={`/tin-tuc/${p.slug}`} className="post reveal">
              <span className="post__date">
                <CalendarIcon width={16} height={16} /> {p.date}
              </span>
              <h3>{p.title}</h3>
              <span className="post__more">Xem thêm →</span>
            </Link>
          ))}
          {posts !== null && items.length === 0 && <p>Chưa có bài viết nào.</p>}
        </div>
      </div>
    </section>
  )
}
