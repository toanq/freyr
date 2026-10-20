import { API_URL, apiFetch } from '../api'

export type Profile = {
  id: string
  username: string
  email: string | null
  role: string
  avatarUrl: string | null
}

// Fired after the avatar changes so the app bar (AppBarAvatar) refetches.
export const PROFILE_CHANGED_EVENT = 'synora:profile-changed'

export function notifyProfileChanged() {
  window.dispatchEvent(new Event(PROFILE_CHANGED_EVENT))
}

export async function fetchProfile(): Promise<Profile> {
  const res = await apiFetch('/users/me')
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return res.json()
}

// The server reuses the same file name per user, so append a version to defeat
// the browser cache after an upload.
export function avatarSrc(avatarUrl: string, version?: number) {
  return `${API_URL}${avatarUrl}${version ? `?v=${version}` : ''}`
}

function postJson(path: string, body: unknown, method = 'POST') {
  return apiFetch(path, {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
}

// `?cookie=true` makes the mobile auth endpoints sign the browser in with the
// web-console cookie instead of returning a JWT.
export const register = (username: string, email: string | null, password: string, rememberMe: boolean) =>
  postJson('/auth/register?cookie=true', { username, email, password, rememberMe })

export const loginWithGoogle = (idToken: string) => postJson('/auth/oauth/google?cookie=true', { idToken })

export const loginWithFacebook = (accessToken: string) => postJson('/auth/oauth/facebook?cookie=true', { accessToken })

export const updateEmail = (email: string | null) => postJson('/users/me', { email }, 'PATCH')

export const changePassword = (currentPassword: string, newPassword: string) =>
  postJson('/users/me/password', { currentPassword, newPassword }, 'PATCH')

export async function uploadAvatar(file: File): Promise<string> {
  const body = new FormData()
  body.append('file', file)
  const res = await apiFetch('/users/me/avatar', { method: 'POST', body })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
  return ((await res.json()) as { avatarUrl: string }).avatarUrl
}
