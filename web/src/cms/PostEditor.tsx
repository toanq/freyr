import { useEffect, useRef, useState, type ChangeEvent, type FormEvent } from 'react'
import { useI18n } from '../i18n'
import RichTextEditor from './RichTextEditor'
import { CmsError, cmsApi, slugify, uploadPreviewUrl, type CmsPostInput } from './cmsApi'

type Form = {
  title: string
  slug: string
  excerpt: string
  content: string
  cover_image: string
  category: string
  tags: string
  published: boolean
}

const EMPTY: Form = { title: '', slug: '', excerpt: '', content: '', cover_image: '', category: '', tags: '', published: true }

const errorText = (err: unknown) => (err instanceof CmsError ? err.message : String(err))

function PostEditor({ postId, onDone }: { postId: string | null; onDone: () => void }) {
  const { t } = useI18n()
  const isNew = postId === null
  const [form, setForm] = useState<Form>(EMPTY)
  const [loaded, setLoaded] = useState(isNew)
  const [slugTouched, setSlugTouched] = useState(!isNew)
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)
  const [uploading, setUploading] = useState(false)
  const coverInput = useRef<HTMLInputElement>(null)

  useEffect(() => {
    if (postId === null) return
    let cancelled = false
    cmsApi
      .getPost(postId)
      .then((p) => {
        if (cancelled) return
        setForm({
          title: p.title,
          slug: p.slug,
          excerpt: p.excerpt,
          content: p.content,
          cover_image: p.cover_image ?? '',
          category: p.category ?? '',
          tags: p.tags,
          published: p.published,
        })
        setLoaded(true)
      })
      .catch(() => !cancelled && setError(t('cms.editor.loadFailed')))
    return () => {
      cancelled = true
    }
  }, [postId, t])

  const set = <K extends keyof Form>(key: K, value: Form[K]) =>
    setForm((f) => {
      const next = { ...f, [key]: value }
      if (key === 'title' && !slugTouched) next.slug = slugify(String(value))
      return next
    })

  const text = (key: Exclude<keyof Form, 'published'>) => (e: ChangeEvent<HTMLInputElement | HTMLTextAreaElement>) =>
    set(key, e.target.value)

  const uploadCover = async (e: ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0]
    e.target.value = ''
    if (!file) return
    setUploading(true)
    setError(null)
    try {
      const { url } = await cmsApi.uploadImage(file)
      set('cover_image', url)
    } catch (err) {
      setError(t('cms.editor.uploadFailed', { error: errorText(err) }))
    } finally {
      setUploading(false)
    }
  }

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setSaving(true)
    setError(null)
    const payload: CmsPostInput = {
      ...form,
      title: form.title.trim(),
      slug: form.slug.trim(),
      cover_image: form.cover_image.trim() || null,
      category: form.category.trim() || null,
    }
    try {
      if (postId === null) await cmsApi.createPost(payload)
      else await cmsApi.updatePost(postId, payload)
      onDone()
    } catch (err) {
      setError(t('cms.editor.saveFailed', { error: errorText(err) }))
    } finally {
      setSaving(false)
    }
  }

  return (
    <section className="cms cms-editor">
      <div className="page-header">
        <button type="button" onClick={onDone}>
          {t('cms.editor.back')}
        </button>
        <h2>{isNew ? t('cms.editor.new') : t('cms.editor.edit')}</h2>
      </div>

      {!loaded && !error && <p className="empty-state">{t('cms.editor.loadingEditor')}</p>}
      {loaded && (
        <form className="card cms-form" onSubmit={submit}>
          <label className="cms-field">
            <span>{t('cms.editor.title')}</span>
            <input required value={form.title} onChange={text('title')} />
          </label>
          <label className="cms-field">
            <span>{t('cms.editor.slug')}</span>
            <input
              required
              value={form.slug}
              onChange={(e) => {
                setSlugTouched(true)
                set('slug', e.target.value)
              }}
            />
          </label>
          <label className="cms-field">
            <span>{t('cms.editor.excerpt')}</span>
            <textarea rows={2} value={form.excerpt} onChange={text('excerpt')} />
          </label>
          <div className="cms-field">
            <span>{t('cms.editor.content')}</span>
            <RichTextEditor value={form.content} onChange={(html) => set('content', html)} onError={setError} />
          </div>
          <div className="cms-field">
            <span>{t('cms.editor.cover')}</span>
            <div className="cms-cover-row">
              <input value={form.cover_image} onChange={text('cover_image')} placeholder="/uploads/..." />
              <button type="button" disabled={uploading} onClick={() => coverInput.current?.click()}>
                {uploading ? t('cms.editor.uploading') : t('cms.editor.upload')}
              </button>
              <input ref={coverInput} type="file" accept="image/*" hidden onChange={(e) => void uploadCover(e)} />
            </div>
            {form.cover_image && (
              <div className="cms-cover-preview">
                <img src={uploadPreviewUrl(form.cover_image)} alt="" />
                <button type="button" className="btn-danger" onClick={() => set('cover_image', '')}>
                  {t('cms.editor.removeCover')}
                </button>
              </div>
            )}
          </div>
          <label className="cms-field">
            <span>{t('cms.editor.category')}</span>
            <input value={form.category} onChange={text('category')} />
          </label>
          <label className="cms-field">
            <span>{t('cms.editor.tags')}</span>
            <input value={form.tags} onChange={text('tags')} placeholder={t('cms.editor.tagsPlaceholder')} />
          </label>
          <label className="cms-check">
            <input type="checkbox" checked={form.published} onChange={(e) => set('published', e.target.checked)} />
            {t('cms.editor.published')}
          </label>
          <button type="submit" disabled={saving || uploading}>
            {saving ? t('cms.editor.saving') : t('cms.editor.save')}
          </button>
        </form>
      )}
      {error && <p className="form-error">{error}</p>}
    </section>
  )
}

export default PostEditor
