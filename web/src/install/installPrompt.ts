// Chromium fires `beforeinstallprompt` once, possibly before the user has
// signed in (i.e. before the banner is mounted), so it is captured at startup
// and handed to whichever component subscribes later.

export type BeforeInstallPromptEvent = Event & {
  prompt: () => Promise<void>
  userChoice: Promise<{ outcome: 'accepted' | 'dismissed' }>
}

let deferred: BeforeInstallPromptEvent | null = null
let installed = false
const listeners = new Set<() => void>()

function notify() {
  listeners.forEach((l) => l())
}

export function initInstallPrompt() {
  window.addEventListener('beforeinstallprompt', (e) => {
    // Keep Chrome's mini-infobar from appearing; we show our own suggestion.
    e.preventDefault()
    deferred = e as BeforeInstallPromptEvent
    notify()
  })
  window.addEventListener('appinstalled', () => {
    deferred = null
    installed = true
    notify()
  })
}

export function canPromptInstall(): boolean {
  return deferred !== null
}

export function wasJustInstalled(): boolean {
  return installed
}

export async function promptInstall(): Promise<boolean> {
  if (!deferred) return false
  const event = deferred
  deferred = null
  await event.prompt()
  const { outcome } = await event.userChoice
  notify()
  return outcome === 'accepted'
}

export function subscribeInstall(listener: () => void): () => void {
  listeners.add(listener)
  return () => {
    listeners.delete(listener)
  }
}
