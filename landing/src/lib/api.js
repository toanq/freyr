export const API_URL = import.meta.env.VITE_API_URL ?? '/api'

export class ApiError extends Error {
  constructor(status, message) {
    super(message)
    this.status = status
  }
}

export async function apiFetch(path, init) {
  const isFormData = init?.body instanceof FormData
  const res = await fetch(`${API_URL}${path}`, {
    ...init,
    credentials: 'include',
    headers: isFormData ? init?.headers : { 'Content-Type': 'application/json', ...init?.headers },
  })
  if (!res.ok) {
    const body = await res.text().catch(() => '')
    throw new ApiError(res.status, body || `Request failed (${res.status})`)
  }
  if (res.status === 204) return null
  return res.json()
}

export const api = {
  listPosts: () => apiFetch('/posts'),
  getPost: (slug) => apiFetch(`/posts/${slug}`),
  createOrder: (order) => apiFetch('/orders', { method: 'POST', body: JSON.stringify(order) }),
}
