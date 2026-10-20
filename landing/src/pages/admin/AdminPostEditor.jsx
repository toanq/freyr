import { useEffect, useState } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import { ApiError, api } from '../../lib/api.js'
import RichTextEditor from '../../components/admin/RichTextEditor.jsx'
import Breadcrumb from '../../components/admin/Breadcrumb.jsx'

const EMPTY = { slug: '', title: '', excerpt: '', content: '', cover_image: '', category: '', tags: '', published: true }

function slugify(text) {
  return text
    .toLowerCase()
    .normalize('NFD')
    .replace(/[̀-ͯ]/g, '')
    .replace(/đ/g, 'd')
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
}

export default function AdminPostEditor() {
  const { id } = useParams()
  const navigate = useNavigate()
  const isNew = id === undefined
  const [form, setForm] = useState(EMPTY)
  const [slugTouched, setSlugTouched] = useState(false)
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  const [uploading, setUploading] = useState(false)

  const uploadCover = async (e) => {
    const file = e.target.files?.[0]
    e.target.value = ''
    if (!file) return
    setUploading(true)
    try {
      const { url } = await api.uploadImage(file)
      setForm((f) => ({ ...f, cover_image: url }))
    } catch {
      setError('Không thể tải ảnh lên.')
    } finally {
      setUploading(false)
    }
  }

  useEffect(() => {
    if (isNew) {
      setForm(EMPTY)
      setSlugTouched(false)
      return
    }
    api.getAdminPost(id).then((post) => {
      setForm({ ...post, cover_image: post.cover_image ?? '', category: post.category ?? '' })
      setSlugTouched(true)
    })
  }, [id, isNew])

  const update = (k) => (e) => {
    const value = e.target.type === 'checkbox' ? e.target.checked : e.target.value
    setForm((f) => {
      const next = { ...f, [k]: value }
      if (k === 'title' && !slugTouched) next.slug = slugify(value)
      return next
    })
  }

  const submit = async (e) => {
    e.preventDefault()
    setBusy(true)
    setError('')
    const payload = { ...form, cover_image: form.cover_image.trim() || null, category: form.category.trim() || null }
    try {
      if (isNew) {
        await api.createPost(payload)
      } else {
        await api.updatePost(id, payload)
      }
      navigate('/admin/posts')
    } catch (err) {
      setError(err instanceof ApiError ? err.message : 'Không thể lưu bài viết.')
    } finally {
      setBusy(false)
    }
  }

  return (
    <div>
      <Breadcrumb items={[{ label: 'Bài viết', to: '/admin/posts' }, { label: isNew ? 'Bài viết mới' : 'Sửa bài viết' }]} />
      <h1>{isNew ? 'Bài viết mới' : 'Sửa bài viết'}</h1>
      <form className="admin-form" onSubmit={submit}>
        <div className="field">
          <label htmlFor="post-title">Tiêu đề</label>
          <input id="post-title" required value={form.title} onChange={update('title')} />
        </div>
        <div className="field">
          <label htmlFor="post-slug">Slug (đường dẫn)</label>
          <input
            id="post-slug"
            required
            value={form.slug}
            onChange={(e) => {
              setSlugTouched(true)
              update('slug')(e)
            }}
          />
        </div>
        <div className="field">
          <label htmlFor="post-excerpt">Tóm tắt</label>
          <textarea id="post-excerpt" rows={2} value={form.excerpt} onChange={update('excerpt')} />
        </div>
        <div className="field">
          <label htmlFor="post-content">Nội dung</label>
          <RichTextEditor value={form.content} onChange={(html) => setForm((f) => ({ ...f, content: html }))} />
        </div>
        <div className="field">
          <label htmlFor="post-cover">Ảnh bìa (URL)</label>
          <input id="post-cover" value={form.cover_image} onChange={update('cover_image')} />
          <label className="btn btn--ghost admin-form__upload">
            {uploading ? 'Đang tải...' : 'Tải ảnh lên'}
            <input type="file" accept="image/*" hidden disabled={uploading} onChange={uploadCover} />
          </label>
          {form.cover_image && <img src={form.cover_image} alt="" className="admin-form__cover-preview" />}
        </div>
        <div className="field">
          <label htmlFor="post-category">Danh mục</label>
          <input id="post-category" value={form.category} onChange={update('category')} />
        </div>
        <div className="field">
          <label htmlFor="post-tags">Thẻ (cách nhau bởi dấu phẩy)</label>
          <input id="post-tags" value={form.tags} onChange={update('tags')} placeholder="ưu đãi, sản phẩm mới" />
        </div>
        <label className="admin-form__checkbox">
          <input type="checkbox" checked={form.published} onChange={update('published')} /> Đăng ngay
        </label>
        <div className="form-actions">
          <button type="submit" className="btn btn--primary" disabled={busy}>
            {busy ? 'Đang lưu...' : 'Lưu bài viết'}
          </button>
        </div>
        {error && <p className="form-note form-note--error">{error}</p>}
      </form>
    </div>
  )
}
