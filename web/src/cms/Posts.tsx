import { Suspense, lazy, useCallback, useEffect, useState } from 'react'
import { useParamRoute } from '../device/deviceRoute'
import { useI18n } from '../i18n'
import { CmsError, cmsApi, type CmsPost } from './cmsApi'
import './cms.css'

// TipTap + ProseMirror are heavy; only admins who open the editor load them.
const PostEditor = lazy(() => import('./PostEditor'))

function Posts() {
  const { t, formatDate } = useI18n()
  const [postParam, openPost, closePost] = useParamRoute('post')
  const [posts, setPosts] = useState<CmsPost[] | null>(null)
  const [loadFailed, setLoadFailed] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [busyId, setBusyId] = useState<string | null>(null)

  const load = useCallback(() => {
    cmsApi
      .listPosts()
      .then((list) => {
        setPosts(list)
        setLoadFailed(false)
      })
      .catch(() => setLoadFailed(true))
  }, [])

  useEffect(() => {
    if (!postParam) load()
  }, [postParam, load])

  if (postParam) {
    return (
      <Suspense fallback={<p className="empty-state">{t('cms.editor.loadingEditor')}</p>}>
        <PostEditor postId={postParam === 'new' ? null : postParam} onDone={closePost} />
      </Suspense>
    )
  }

  const run = async (id: string, action: () => Promise<unknown>) => {
    setError(null)
    setBusyId(id)
    try {
      await action()
      load()
    } catch (err) {
      setError(t('cms.posts.actionFailed', { error: err instanceof CmsError ? err.message : String(err) }))
    } finally {
      setBusyId(null)
    }
  }

  const togglePublished = (p: CmsPost) =>
    run(p.id, () =>
      cmsApi.updatePost(p.id, {
        slug: p.slug,
        title: p.title,
        excerpt: p.excerpt,
        content: p.content,
        cover_image: p.cover_image,
        category: p.category,
        tags: p.tags,
        published: !p.published,
      }),
    )

  const remove = (p: CmsPost) => {
    if (!window.confirm(t('cms.posts.confirmDelete', { title: p.title }))) return
    void run(p.id, () => cmsApi.deletePost(p.id))
  }

  return (
    <section className="cms">
      <div className="page-header">
        <h2>{t('tab.posts')}</h2>
        <span className="page-count">{posts?.length ?? 0}</span>
        <button type="button" className="btn-primary cms-new" onClick={() => openPost('new')}>
          {t('cms.posts.new')}
        </button>
      </div>

      {loadFailed && <p className="form-error">{t('cms.posts.loadFailed')}</p>}
      {error && <p className="form-error">{error}</p>}
      {posts && posts.length === 0 && <p className="empty-state">{t('cms.posts.empty')}</p>}

      <ul className="cms-list">
        {posts?.map((p) => (
          <li key={p.id} className="card cms-post">
            <div className="cms-post-head">
              <button type="button" className="cms-post-title" onClick={() => openPost(p.id)}>
                {p.title}
              </button>
              <span className={p.published ? 'cms-status cms-status--done' : 'cms-status cms-status--draft'}>
                {p.published ? t('cms.posts.published') : t('cms.posts.draft')}
              </span>
            </div>
            <p className="cms-post-meta">
              <code>/tin-tuc/{p.slug}</code>
              {p.category && <span className="chip">{p.category}</span>}
            </p>
            <p className="cms-post-meta">{t('cms.posts.updated', { date: formatDate(p.updated_at) })}</p>
            <div className="cms-post-actions">
              <button type="button" onClick={() => openPost(p.id)}>
                {t('cms.posts.edit')}
              </button>
              <button type="button" disabled={busyId === p.id} onClick={() => void togglePublished(p)}>
                {p.published ? t('cms.posts.unpublish') : t('cms.posts.publish')}
              </button>
              {p.published && (
                <a className="cms-link" href={`/tin-tuc/${encodeURIComponent(p.slug)}`} target="_blank" rel="noreferrer">
                  {t('cms.posts.view')}
                </a>
              )}
              <button type="button" className="btn-danger" disabled={busyId === p.id} onClick={() => remove(p)}>
                {t('cms.posts.delete')}
              </button>
            </div>
          </li>
        ))}
      </ul>
    </section>
  )
}

export default Posts
