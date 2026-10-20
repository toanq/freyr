import { useCallback, useEffect, useState } from 'react'

// A query param in the URL selects a sub-page (?device=<id> for the per-device
// page, ?post=<id|new> for the CMS editor). pushState/popstate keep browser
// and Android back working and let a reload reopen the same page, without
// pulling in a router.
function readParam(param: string): string | null {
  return new URLSearchParams(window.location.search).get(param)
}

function urlWith(param: string, value: string | null): string {
  const url = new URL(window.location.href)
  if (value) url.searchParams.set(param, value)
  else url.searchParams.delete(param)
  return `${url.pathname}${url.search}${url.hash}`
}

export function useParamRoute(param: string): [string | null, (value: string) => void, () => void] {
  const [value, setValue] = useState<string | null>(() => readParam(param))

  useEffect(() => {
    const onPop = () => setValue(readParam(param))
    window.addEventListener('popstate', onPop)
    return () => window.removeEventListener('popstate', onPop)
  }, [param])

  const open = useCallback(
    (next: string) => {
      window.history.pushState({ synoraSubPage: param }, '', urlWith(param, next))
      setValue(next)
      window.scrollTo(0, 0)
    },
    [param],
  )

  const close = useCallback(() => {
    // Opened from the list in this tab: pop our own history entry so the
    // back stack stays clean. Opened directly (reload / shared link): just
    // drop the param.
    if ((window.history.state as { synoraSubPage?: string } | null)?.synoraSubPage === param) {
      window.history.back()
    } else {
      window.history.replaceState(null, '', urlWith(param, null))
      setValue(null)
    }
  }, [param])

  return [value, open, close]
}

export function useDeviceRoute(): [string | null, (id: string) => void, () => void] {
  return useParamRoute('device')
}
