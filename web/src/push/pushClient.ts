import { apiFetch } from '../api'
import { isIos, isStandalone } from '../platform'

// Web push via Firebase Cloud Messaging. The Firebase SDK is only loaded when
// a push action runs; sw.js handles incoming pushes itself (no SDK there).

const env = import.meta.env
const firebaseConfig = {
  apiKey: env.VITE_FIREBASE_API_KEY as string | undefined,
  projectId: env.VITE_FIREBASE_PROJECT_ID as string | undefined,
  messagingSenderId: env.VITE_FIREBASE_MESSAGING_SENDER_ID as string | undefined,
  appId: env.VITE_FIREBASE_APP_ID as string | undefined,
}
const vapidKey = env.VITE_FIREBASE_VAPID_KEY as string | undefined

// Token registered from this browser, so sign-out/opt-out can remove it server-side.
const TOKEN_KEY = 'synora.pushToken'

export type PushStatus = 'unsupported' | 'needs-install' | 'blocked' | 'off' | 'on'

export function isPushConfigured(): boolean {
  return Boolean(
    firebaseConfig.apiKey && firebaseConfig.projectId && firebaseConfig.messagingSenderId && firebaseConfig.appId && vapidKey,
  )
}

function storedToken(): string | null {
  try {
    return localStorage.getItem(TOKEN_KEY)
  } catch {
    return null
  }
}

function storeToken(token: string | null) {
  try {
    if (token) localStorage.setItem(TOKEN_KEY, token)
    else localStorage.removeItem(TOKEN_KEY)
  } catch {
    // best effort: without storage the token just can't be removed on sign-out
  }
}

// iOS only offers web push to the installed home-screen app (16.4+).
export async function getPushStatus(): Promise<PushStatus> {
  if (isIos() && !isStandalone()) return 'needs-install'
  if (!('serviceWorker' in navigator) || !('PushManager' in window) || !('Notification' in window)) return 'unsupported'
  const { isSupported } = await import('firebase/messaging')
  if (!(await isSupported())) return 'unsupported'
  if (Notification.permission === 'denied') return 'blocked'
  return Notification.permission === 'granted' && storedToken() ? 'on' : 'off'
}

async function messaging() {
  const [{ initializeApp, getApps }, { getMessaging }] = await Promise.all([
    import('firebase/app'),
    import('firebase/messaging'),
  ])
  const app = getApps()[0] ?? initializeApp(firebaseConfig)
  return getMessaging(app)
}

async function fetchToken(): Promise<string> {
  const { getToken } = await import('firebase/messaging')
  const registration = await navigator.serviceWorker.ready
  return getToken(await messaging(), { vapidKey, serviceWorkerRegistration: registration })
}

async function registerWithServer(token: string) {
  const res = await apiFetch('/users/me/fcm-token', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ token, platform: 'web' }),
  })
  if (!res.ok) throw new Error(`HTTP ${res.status}`)
}

async function unregisterWithServer(token: string) {
  await apiFetch(`/users/me/fcm-token?token=${encodeURIComponent(token)}`, { method: 'DELETE' })
}

// Must run from a user gesture: it may show the browser's permission prompt.
export async function enablePush(): Promise<PushStatus> {
  const permission = await Notification.requestPermission()
  if (permission !== 'granted') return permission === 'denied' ? 'blocked' : 'off'
  const token = await fetchToken()
  await registerWithServer(token)
  storeToken(token)
  return 'on'
}

export async function disablePush(): Promise<void> {
  const token = storedToken()
  storeToken(null)
  if (!token) return
  await unregisterWithServer(token).catch(() => {})
  try {
    const { deleteToken } = await import('firebase/messaging')
    await deleteToken(await messaging())
  } catch {
    // server-side removal is what matters; the browser subscription can linger
  }
}

// FCM web tokens can rotate without notice; re-check on each app start and
// re-register when it changed. No-op unless this browser opted in.
export async function refreshPushToken(): Promise<void> {
  if (!isPushConfigured() || !storedToken()) return
  try {
    if ((await getPushStatus()) !== 'on') return
    const token = await fetchToken()
    if (token !== storedToken()) {
      await registerWithServer(token)
      storeToken(token)
    }
  } catch {
    // keep the old token; next start tries again
  }
}

// Before sign-out: stop this browser receiving the signed-out user's alerts.
export async function forgetPushOnSignOut(): Promise<void> {
  if (!isPushConfigured()) return
  await disablePush().catch(() => {})
}
