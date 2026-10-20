import { useCallback, useEffect, useMemo, useState, type ReactNode } from 'react'
import { I18nContext, strings, useI18n, type I18n, type Lang } from './i18n'

const STORAGE_KEY = 'synora.lang'

function initialLang(): Lang {
  try {
    const saved = localStorage.getItem(STORAGE_KEY)
    if (saved === 'vi' || saved === 'en') return saved
  } catch {
    // storage unavailable (private mode etc.) -- fall back to the default
  }
  return 'vi'
}

export function I18nProvider({ children }: { children: ReactNode }) {
  const [lang, setLangState] = useState<Lang>(initialLang)

  useEffect(() => {
    document.documentElement.lang = lang
  }, [lang])

  const setLang = useCallback((next: Lang) => {
    setLangState(next)
    try {
      localStorage.setItem(STORAGE_KEY, next)
    } catch {
      // not persisted; still applies for this session
    }
  }, [])

  const value = useMemo<I18n>(
    () => ({
      lang,
      setLang,
      t: (key, vars) =>
        strings[key][lang].replace(/\{(\w+)\}/g, (match, name: string) =>
          vars && name in vars ? String(vars[name]) : match,
        ),
      formatDate: (iso) => new Date(iso).toLocaleString(lang === 'vi' ? 'vi-VN' : 'en-US'),
    }),
    [lang, setLang],
  )

  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>
}

export function LanguageToggle() {
  const { lang, setLang, t } = useI18n()
  return (
    <div className="lang-toggle" role="group" aria-label={t('shell.language')}>
      {(['vi', 'en'] as const).map((l) => (
        <button
          key={l}
          type="button"
          className={lang === l ? 'lang-option lang-option--active' : 'lang-option'}
          aria-pressed={lang === l}
          onClick={() => setLang(l)}
        >
          {l.toUpperCase()}
        </button>
      ))}
    </div>
  )
}
