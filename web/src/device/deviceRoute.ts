import { useCallback, useEffect, useState } from 'react'

// ?device=<id> in the URL selects the per-device page. pushState/popstate keep
// browser and Android back working and let a reload reopen the same device,
// without pulling in a router.
const PARAM = 'device'

function readDeviceId(): string | null {
  return new URLSearchParams(window.location.search).get(PARAM)
}

function urlWith(id: string | null): string {
  const url = new URL(window.location.href)
  if (id) url.searchParams.set(PARAM, id)
  else url.searchParams.delete(PARAM)
  return `${url.pathname}${url.search}${url.hash}`
}

export function useDeviceRoute(): [string | null, (id: string) => void, () => void] {
  const [deviceId, setDeviceId] = useState<string | null>(readDeviceId)

  useEffect(() => {
    const onPop = () => setDeviceId(readDeviceId())
    window.addEventListener('popstate', onPop)
    return () => window.removeEventListener('popstate', onPop)
  }, [])

  const open = useCallback((id: string) => {
    window.history.pushState({ synoraDevicePage: true }, '', urlWith(id))
    setDeviceId(id)
    window.scrollTo(0, 0)
  }, [])

  const close = useCallback(() => {
    // Opened from the list in this tab: pop our own history entry so the
    // back stack stays clean. Opened directly (reload / shared link): just
    // drop the param.
    if ((window.history.state as { synoraDevicePage?: boolean } | null)?.synoraDevicePage) {
      window.history.back()
    } else {
      window.history.replaceState(null, '', urlWith(null))
      setDeviceId(null)
    }
  }, [])

  return [deviceId, open, close]
}
