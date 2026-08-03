export const API_URL = import.meta.env.VITE_API_URL ?? 'http://localhost:5080'

export function apiFetch(path: string, init?: RequestInit) {
  return fetch(`${API_URL}${path}`, { ...init, credentials: 'include' })
}
