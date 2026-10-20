import { API_URL, apiFetch } from '../api'

// Landing-site CMS admin endpoints (server/Synora.Api/Cms/CmsEndpoints.cs),
// reached through the console's /api base as /api/cms/admin/*. They accept the
// console's Synora session cookie and require Admin/SuperAdmin. JSON is
// snake_case; error bodies are plain text.

export const ORDER_STATUSES = ['new', 'confirmed', 'shipped', 'done', 'cancelled'] as const
export type OrderStatus = (typeof ORDER_STATUSES)[number]

export type CmsOrder = {
  id: string
  customer_name: string
  phone: string
  email: string | null
  address: string | null
  quantity: number
  note: string | null
  status: OrderStatus
  created_at: string
}

export type CmsPost = {
  id: string
  slug: string
  title: string
  excerpt: string
  content: string
  cover_image: string | null
  category: string | null
  tags: string
  published: boolean
  published_at: string | null
  created_at: string
  updated_at: string
}

export type CmsPostInput = Pick<
  CmsPost,
  'slug' | 'title' | 'excerpt' | 'content' | 'cover_image' | 'category' | 'tags' | 'published'
>

export class CmsError extends Error {
  readonly status: number
  constructor(status: number, message: string) {
    super(message)
    this.status = status
  }
}

async function call<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await apiFetch(`/cms/admin${path}`, init)
  if (!res.ok) {
    const text = await res.text().catch(() => '')
    throw new CmsError(res.status, text || `HTTP ${res.status}`)
  }
  if (res.status === 204) return undefined as T
  return (await res.json()) as T
}

const json = (method: string, body: unknown): RequestInit => ({
  method,
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify(body),
})

export const cmsApi = {
  listOrders: () => call<CmsOrder[]>('/orders'),
  setOrderStatus: (id: string, status: OrderStatus) => call<CmsOrder>(`/orders/${id}/status`, json('POST', { status })),
  listPosts: () => call<CmsPost[]>('/posts'),
  getPost: (id: string) => call<CmsPost>(`/posts/${id}`),
  createPost: (post: CmsPostInput) => call<CmsPost>('/posts', json('POST', post)),
  updatePost: (id: string, post: CmsPostInput) => call<CmsPost>(`/posts/${id}`, json('PUT', post)),
  deletePost: (id: string) => call<void>(`/posts/${id}`, { method: 'DELETE' }),
  uploadImage: (file: File) => {
    const body = new FormData()
    body.append('file', file)
    return call<{ url: string }>('/uploads', { method: 'POST', body })
  },
}

// Uploads come back as "/uploads/<file>" -- served at the site root in
// production (Traefik -> server /cms/uploads). Against a separate dev API
// origin there is no Traefik, so point previews at the server directly.
export function uploadPreviewUrl(url: string): string {
  if (url.startsWith('/uploads/') && /^https?:\/\//.test(API_URL)) {
    return `${API_URL.replace(/\/$/, '')}/cms${url}`
  }
  return url
}

export function slugify(text: string): string {
  return text
    .toLowerCase()
    .normalize('NFD')
    .replace(/[̀-ͯ]/g, '')
    .replace(/đ/g, 'd')
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
}
