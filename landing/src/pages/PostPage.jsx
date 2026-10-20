import { useEffect, useState } from 'react'
import { useParams } from 'react-router-dom'
import Breadcrumb from '../components/Breadcrumb.jsx'
import Footer from '../components/Footer.jsx'
import PageHeader from '../components/PageHeader.jsx'
import { api } from '../lib/api.js'

function shareOnFacebook() {
  const url = `https://www.facebook.com/sharer/sharer.php?u=${encodeURIComponent(window.location.href)}`
  window.open(url, 'facebook-share', 'width=580,height=480,noopener,noreferrer')
}

export default function PostPage() {
  const { slug } = useParams()
  const [post, setPost] = useState(null)
  const [notFound, setNotFound] = useState(false)

  useEffect(() => {
    setPost(null)
    setNotFound(false)
    api
      .getPost(slug)
      .then(setPost)
      .catch(() => setNotFound(true))
  }, [slug])

  const tags = post?.tags ? post.tags.split(',').map((t) => t.trim()).filter(Boolean) : []

  return (
    <>
      <PageHeader />
      <div className="post-page">
        <Breadcrumb items={[{ label: 'Tin tức', to: '/tin-tuc' }, { label: post?.title ?? 'Bài viết' }]} />
        <div className="container post-page__body">
          {notFound && <p>Không tìm thấy bài viết.</p>}
          {!notFound && post && (
            <article>
              <p className="eyebrow">{post.category || 'TIN TỨC'}</p>
              <h1 className="section__title">{post.title}</h1>
              <button type="button" className="post-page__share" onClick={shareOnFacebook}>
                Chia sẻ lên Facebook
              </button>
              {post.cover_image && <img className="post-page__cover" src={post.cover_image} alt="" />}
              <div className="post-page__content" dangerouslySetInnerHTML={{ __html: post.content }} />
              {tags.length > 0 && (
                <div className="post-page__tags">
                  {tags.map((tag) => (
                    <span key={tag} className="post-page__tag">
                      #{tag}
                    </span>
                  ))}
                </div>
              )}
            </article>
          )}
        </div>
      </div>
      <Footer />
    </>
  )
}
