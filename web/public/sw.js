// App-shell cache only. API calls (/api/*) and APK downloads always go to the
// network so device data and auth state are never served stale.
const CACHE = 'synora-shell-v1'
const SHELL = ['/', '/manifest.webmanifest', '/logo-green.jpg', '/icons/icon-192.png', '/icons/icon-512.png']

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(SHELL)))
  self.skipWaiting()
})

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  )
})

self.addEventListener('fetch', (event) => {
  const { request } = event
  if (request.method !== 'GET') return
  const url = new URL(request.url)
  if (url.origin !== self.location.origin) return
  if (url.pathname.startsWith('/api/') || url.pathname.startsWith('/downloads/')) return

  // Navigations: network-first so a redeploy shows up immediately, cached
  // shell as the offline fallback.
  if (request.mode === 'navigate') {
    event.respondWith(
      fetch(request)
        .then((res) => {
          const copy = res.clone()
          caches.open(CACHE).then((cache) => cache.put('/', copy))
          return res
        })
        .catch(() => caches.match('/')),
    )
    return
  }

  // Hashed build assets and icons: cache-first, filled on first use.
  event.respondWith(
    caches.match(request).then(
      (cached) =>
        cached ??
        fetch(request).then((res) => {
          if (res.ok) {
            const copy = res.clone()
            caches.open(CACHE).then((cache) => cache.put(request, copy))
          }
          return res
        }),
    ),
  )
})

// Web push (FCM). The page's Firebase SDK only subscribes; delivery is handled
// here directly so this worker needs no Firebase SDK or config. FCM wraps the
// message as { notification, data, fcmOptions: { link } }.
self.addEventListener('push', (event) => {
  let payload = {}
  try {
    payload = event.data ? event.data.json() : {}
  } catch {
    payload = { notification: { body: event.data ? event.data.text() : '' } }
  }
  const notification = payload.notification || {}
  const data = payload.data || {}
  const link =
    (payload.fcmOptions && payload.fcmOptions.link) || (data.deviceId ? `/?device=${encodeURIComponent(data.deviceId)}` : '/')
  event.waitUntil(
    self.registration.showNotification(notification.title || 'Synora', {
      body: notification.body || '',
      icon: notification.icon || '/icons/icon-192.png',
      badge: '/icons/icon-192.png',
      tag: data.deviceId ? `device-${data.deviceId}` : undefined,
      data: { link },
    }),
  )
})

self.addEventListener('notificationclick', (event) => {
  event.notification.close()
  const target = new URL((event.notification.data && event.notification.data.link) || '/', self.location.origin)
  // Never navigate off-site, whatever the payload says.
  const url = target.origin === self.location.origin ? target.href : self.location.origin + '/'
  event.waitUntil(
    self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then((windows) => {
      const open = windows.find((w) => new URL(w.url).origin === self.location.origin)
      if (open) return open.navigate(url).then((w) => (w || open).focus())
      return self.clients.openWindow(url)
    }),
  )
})
