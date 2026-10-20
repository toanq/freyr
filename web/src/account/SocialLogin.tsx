import { useEffect, useRef, useState } from 'react'
import { useI18n } from '../i18n'
import { loginWithFacebook, loginWithGoogle } from './accountApi'

// Build-time ids (web/Dockerfile ARGs). The Google one must be a *web* OAuth
// client, accepted server-side via OAuth__Google__WebClientId.
const GOOGLE_CLIENT_ID: string = import.meta.env.VITE_GOOGLE_CLIENT_ID ?? ''
const FACEBOOK_APP_ID: string = import.meta.env.VITE_FACEBOOK_APP_ID ?? ''

type GoogleId = {
  initialize: (config: { client_id: string; callback: (response: { credential?: string }) => void }) => void
  renderButton: (parent: HTMLElement, options: Record<string, unknown>) => void
}
type FacebookSdk = {
  init: (config: { appId: string; version: string; cookie?: boolean; xfbml?: boolean }) => void
  login: (callback: (response: { authResponse?: { accessToken: string } | null }) => void, options?: { scope: string }) => void
}
declare global {
  interface Window {
    google?: { accounts: { id: GoogleId } }
    FB?: FacebookSdk
  }
}

const scriptLoads = new Map<string, Promise<void>>()

function loadScript(src: string): Promise<void> {
  let load = scriptLoads.get(src)
  if (!load) {
    load = new Promise((resolve, reject) => {
      const script = document.createElement('script')
      script.src = src
      script.async = true
      script.onload = () => resolve()
      script.onerror = () => reject(new Error(`Failed to load ${src}`))
      document.head.appendChild(script)
    })
    scriptLoads.set(src, load)
  }
  return load
}

function SocialLogin({ onLoggedIn }: { onLoggedIn: () => void }) {
  const { t, lang } = useI18n()
  const [error, setError] = useState<string | null>(null)
  const googleButton = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!GOOGLE_CLIENT_ID) return
    let cancelled = false
    loadScript('https://accounts.google.com/gsi/client')
      .then(() => {
        const google = window.google?.accounts.id
        if (cancelled || !google || !googleButton.current) return
        google.initialize({
          client_id: GOOGLE_CLIENT_ID,
          callback: async ({ credential }) => {
            const res = credential ? await loginWithGoogle(credential) : null
            if (res?.ok) onLoggedIn()
            else setError(t('account.socialFailed', { provider: 'Google' }))
          },
        })
        googleButton.current.replaceChildren()
        google.renderButton(googleButton.current, {
          theme: 'filled_black',
          size: 'large',
          shape: 'pill',
          width: 300,
          locale: lang,
        })
      })
      .catch(() => setError(t('account.socialFailed', { provider: 'Google' })))
    return () => {
      cancelled = true
    }
  }, [lang, onLoggedIn, t])

  const handleFacebook = async () => {
    setError(null)
    try {
      await loadScript('https://connect.facebook.net/en_US/sdk.js')
      const fb = window.FB
      if (!fb) throw new Error('Facebook SDK unavailable')
      fb.init({ appId: FACEBOOK_APP_ID, version: 'v19.0' })
      fb.login(
        async (response) => {
          const token = response.authResponse?.accessToken
          const res = token ? await loginWithFacebook(token) : null
          if (res?.ok) onLoggedIn()
          else setError(t('account.socialFailed', { provider: 'Facebook' }))
        },
        { scope: 'email' },
      )
    } catch {
      setError(t('account.socialFailed', { provider: 'Facebook' }))
    }
  }

  if (!GOOGLE_CLIENT_ID && !FACEBOOK_APP_ID) return null

  return (
    <div className="social-login">
      <p className="social-divider">
        <span>{t('account.or')}</span>
      </p>
      {GOOGLE_CLIENT_ID && <div className="google-button" ref={googleButton} />}
      {FACEBOOK_APP_ID && (
        <button type="button" className="facebook-button" onClick={handleFacebook}>
          {t('account.facebook')}
        </button>
      )}
      {error && <p className="login-error">{error}</p>}
    </div>
  )
}

export default SocialLogin
