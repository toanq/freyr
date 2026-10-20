import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import Breadcrumb from '../components/Breadcrumb.jsx'
import { CalendarIcon } from '../components/Icons.jsx'
import Footer from '../components/Footer.jsx'
import PageHeader from '../components/PageHeader.jsx'
import { api } from '../lib/api.js'

function formatDate(iso) {
  return new Date(iso).toLocaleDateString('vi-VN')
}

export default function NewsPage() {
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
  const items = posts?.filter((p) => category === 'all' || p.category === category) ?? []

  return (
    <>
      <PageHeader />
      <div className="post-page">
        <Breadcrumb items={[{ label: 'Tin tức' }]} />
        <div className="container post-page__body">
          <p className="eyebrow">TIN TỨC</p>
          <h1 className="section__title">Tin tức cập nhật</h1>
          {categories.length > 0 && (
            <div className="news__filters">
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
              <Link key={p.slug} to={`/tin-tuc/${p.slug}`} className="post">
                <span className="post__date">
                  <CalendarIcon width={16} height={16} /> {formatDate(p.published_at ?? p.created_at)}
                </span>
                <h3>{p.title}</h3>
                <span className="post__more">Xem thêm →</span>
              </Link>
            ))}
            {posts !== null && items.length === 0 && <p>Chưa có bài viết nào.</p>}
          </div>
        </div>
      </div>
      <Footer />
    </>
  )
}
