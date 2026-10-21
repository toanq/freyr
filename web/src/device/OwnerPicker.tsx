import { useEffect, useState } from 'react'
import { apiFetch } from '../api'
import { useI18n } from '../i18n'
import type { DeviceDetailDto } from './types'
import { describeError } from './types'
import './owner.css'

type LookupUser = { id: string; username: string; email: string | null }

type Props = {
  deviceId: string
  deviceName: string
  currentOwner: string | null
  onClose: () => void
  onChanged: (device: DeviceDetailDto) => void
}

type Pending = { kind: 'assign'; user: LookupUser } | { kind: 'clear' } | null

const MIN_QUERY = 2
const DEBOUNCE_MS = 300

// Admin-only sheet: search accounts (GET /admin/users/lookup) and assign,
// change or clear the device's owner (PUT /admin/devices/{id}/owner).
export default function OwnerPicker({ deviceId, deviceName, currentOwner, onClose, onChanged }: Props) {
  const { t } = useI18n()
  const [query, setQuery] = useState('')
  const [results, setResults] = useState<LookupUser[] | null>(null)
  const [searching, setSearching] = useState(false)
  const [pending, setPending] = useState<Pending>(null)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    const term = query.trim()
    if (term.length < MIN_QUERY) {
      setResults(null)
      return
    }
    let cancelled = false
    const timer = window.setTimeout(async () => {
      setSearching(true)
      try {
        const res = await apiFetch(`/admin/users/lookup?q=${encodeURIComponent(term)}`)
        if (cancelled) return
        if (!res.ok) {
          setError(t('owner.searchFailed', { error: await describeError(res) }))
          return
        }
        setError(null)
        setResults(await res.json())
      } catch (err) {
        if (!cancelled) setError(t('owner.searchFailed', { error: String(err) }))
      } finally {
        if (!cancelled) setSearching(false)
      }
    }, DEBOUNCE_MS)
    return () => {
      cancelled = true
      window.clearTimeout(timer)
    }
  }, [query, t])

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && !saving && onClose()
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [onClose, saving])

  const confirm = async () => {
    if (!pending) return
    setSaving(true)
    setError(null)
    try {
      const res = await apiFetch(`/admin/devices/${deviceId}/owner`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ userId: pending.kind === 'assign' ? pending.user.id : null }),
      })
      if (!res.ok) {
        setError(t('owner.saveFailed', { error: await describeError(res) }))
        return
      }
      onChanged(await res.json())
      onClose()
    } catch (err) {
      setError(t('owner.saveFailed', { error: String(err) }))
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="owner-overlay" role="dialog" aria-modal="true" aria-labelledby="owner-title">
      <div className="owner-sheet card">
        <div className="owner-sheet-header">
          <h3 id="owner-title">{t('owner.title')}</h3>
          <button type="button" className="owner-close" onClick={onClose} disabled={saving} aria-label={t('owner.close')}>
            ✕
          </button>
        </div>
        <p className="owner-current">
          {deviceName} · {currentOwner ? t('owner.current', { owner: currentOwner }) : t('owner.none')}
        </p>

        {pending ? (
          <div className="owner-confirm">
            <p>
              {pending.kind === 'assign'
                ? t('owner.confirmAssign', { device: deviceName, user: pending.user.username })
                : t('owner.confirmClear', { device: deviceName, owner: currentOwner ?? '' })}
            </p>
            {currentOwner && <p className="owner-warning">{t('owner.resetWarning')}</p>}
            <div className="owner-actions">
              <button type="button" onClick={() => setPending(null)} disabled={saving}>
                {t('owner.cancel')}
              </button>
              <button
                type="button"
                className={pending.kind === 'clear' ? 'btn-danger' : 'btn-primary'}
                onClick={confirm}
                disabled={saving}
              >
                {saving ? t('owner.saving') : pending.kind === 'clear' ? t('owner.unlink') : t('owner.assign')}
              </button>
            </div>
          </div>
        ) : (
          <>
            <input
              type="search"
              autoFocus
              placeholder={t('owner.searchPlaceholder')}
              aria-label={t('owner.searchPlaceholder')}
              value={query}
              onChange={(e) => setQuery(e.target.value)}
            />
            {query.trim().length > 0 && query.trim().length < MIN_QUERY && (
              <p className="form-hint">{t('owner.minChars')}</p>
            )}
            {searching && <p className="form-hint">{t('owner.searching')}</p>}
            {results && results.length === 0 && !searching && <p className="form-hint">{t('owner.noResults')}</p>}
            {results && results.length > 0 && (
              <ul className="owner-results">
                {results.map((u) => (
                  <li key={u.id}>
                    <button
                      type="button"
                      className="owner-result"
                      disabled={u.username === currentOwner}
                      onClick={() => setPending({ kind: 'assign', user: u })}
                    >
                      <span className="owner-result-name">{u.username}</span>
                      {u.email && <span className="owner-result-email">{u.email}</span>}
                      {u.username === currentOwner && <span className="chip">{t('owner.isCurrent')}</span>}
                    </button>
                  </li>
                ))}
              </ul>
            )}
            {currentOwner && (
              <button type="button" className="btn-danger owner-unlink" onClick={() => setPending({ kind: 'clear' })}>
                {t('owner.unlink')}
              </button>
            )}
          </>
        )}
        {error && <p className="form-error">{error}</p>}
      </div>
    </div>
  )
}
