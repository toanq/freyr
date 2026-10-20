import { useEffect, useRef, useState } from 'react'
import { apiFetch, API_URL } from '../api'
import { useI18n, type StringKey } from '../i18n'
import { SynoraLink, UserCancelledError, chooseSynoraDevice, holdWakeLock, isBluetoothSupported } from './link'
import { ProvisionError, provisionDevice, readDeviceInfo, requestFreshActivationCode, scanWifiNetworks } from './pairingFlow'
import { absoluteServerUrl, type WifiNetwork } from './protocol'
import './ble.css'

type Step = 'intro' | 'connecting' | 'alreadyPaired' | 'wifiForm' | 'joining' | 'activating' | 'done' | 'error'

const PROVISION_ERRORS: Record<string, StringKey> = {
  wifiTimeout: 'ble.errWifiTimeout',
  wifiError: 'ble.errWifi',
  activationTimeout: 'ble.errActivationTimeout',
  activationError: 'ble.errActivation',
  noCode: 'ble.errNoCode',
}

class PairHttpError extends Error {
  readonly status: number
  constructor(status: number) {
    super(`HTTP ${status}`)
    this.status = status
  }
}

async function pairWithServer(code: string, macAddress?: string) {
  const res = await apiFetch('/devices/pair', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ activationCode: code, macAddress: macAddress ?? null }),
  })
  if (!res.ok) throw new PairHttpError(res.status)
}

// Web Bluetooth port of the phone app's BlePairingScreen: choose device ->
// connect -> (already-paired warning) -> Wi-Fi form -> commit -> wait for
// online/activated -> read activation code -> POST /devices/pair.
function BlePairing({ onClose, onPaired }: { onClose: () => void; onPaired: () => void }) {
  const { t } = useI18n()
  const [step, setStep] = useState<Step>('intro')
  const [error, setError] = useState<string | null>(null)
  const [ssid, setSsid] = useState('')
  const [password, setPassword] = useState('')
  const [showPassword, setShowPassword] = useState(false)
  const [networks, setNetworks] = useState<WifiNetwork[]>([])
  const [scanningWifi, setScanningWifi] = useState(false)
  const [initialScanDone, setInitialScanDone] = useState(false)
  const linkRef = useRef<SynoraLink | null>(null)
  // Bumped to invalidate an in-flight attempt (Cancel, unmount): stale
  // results are ignored instead of clobbering the current step.
  const generation = useRef(0)
  const passwordRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    const gen = generation
    const links = linkRef
    let release: () => void = () => {}
    void holdWakeLock().then((r) => (release = r))
    return () => {
      gen.current++
      release()
      links.current?.close()
    }
  }, [])

  const fail = (message: string) => {
    setError(message)
    setStep('error')
  }

  const rescan = async (link: SynoraLink) => {
    setScanningWifi(true)
    try {
      const found = await scanWifiNetworks(link)
      if (found.length > 0) setNetworks(found) // stale-but-real beats a blank list
    } finally {
      setScanningWifi(false)
    }
  }

  const proceedToWifiForm = (link: SynoraLink) => {
    setStep('wifiForm')
    void rescan(link).finally(() => setInitialScanDone(true))
  }

  const connect = async (link: SynoraLink) => {
    setStep('connecting')
    setError(null)
    try {
      await link.connect()
    } catch (err) {
      fail(
        err instanceof DOMException && err.name === 'NotFoundError'
          ? t('ble.errNotSynora')
          : t('ble.errConnect', { error: String(err instanceof Error ? err.message : err) }),
      )
      return
    }
    const info = await readDeviceInfo(link)
    if (info?.isPaired) setStep('alreadyPaired')
    else proceedToWifiForm(link)
  }

  const findDevice = async () => {
    let device: BluetoothDevice
    try {
      device = await chooseSynoraDevice()
    } catch (err) {
      if (err instanceof UserCancelledError) return
      fail(t('ble.errConnect', { error: String(err instanceof Error ? err.message : err) }))
      return
    }
    linkRef.current?.close()
    linkRef.current = new SynoraLink(device)
    await connect(linkRef.current)
  }

  // "Thử lại": reconnect to the same device (it re-advertises on its own)
  // instead of making the user pick it again.
  const retry = () => {
    if (linkRef.current) void connect(linkRef.current)
    else setStep('intro')
  }

  const describe = (err: unknown) => {
    if (err instanceof ProvisionError) return t(PROVISION_ERRORS[err.reason])
    if (err instanceof PairHttpError) {
      return err.status === 409 ? t('ble.errAlreadyPaired') : t('devices.pairFailed', { status: err.status })
    }
    return err instanceof Error ? err.message : String(err)
  }

  const submitWifi = async (e: React.FormEvent) => {
    e.preventDefault()
    const link = linkRef.current
    if (!link) return
    const mine = ++generation.current
    const stale = () => mine !== generation.current
    setStep('joining')
    try {
      const { code, macAddress } = await provisionDevice(
        link,
        { serverUrl: absoluteServerUrl(API_URL, window.location.origin), ssid, password },
        (stage) => !stale() && setStep(stage),
        stale,
      )
      if (stale()) return
      try {
        await pairWithServer(code, macAddress)
      } catch (err) {
        if (!(err instanceof PairHttpError) || err.status !== 404) throw err
        // Device's cached code points at a deleted server row -- mint a fresh one once.
        const fresh = await requestFreshActivationCode(link)
        if (stale()) return
        if (!fresh) {
          fail(t('ble.errNoFreshCode'))
          return
        }
        await pairWithServer(fresh, macAddress)
      }
      onPaired()
      if (stale()) return
      setStep('done')
      setTimeout(() => {
        if (!stale()) onClose()
      }, 2000)
    } catch (err) {
      if (!stale()) fail(describe(err))
    }
  }

  const cancelAttempt = () => {
    generation.current++
    setError(null)
    setStep('wifiForm')
  }

  return (
    <div className="ble-overlay" role="dialog" aria-modal="true" aria-labelledby="ble-pair-title">
      <div className="ble-sheet card">
        <div className="ble-sheet-header">
          <h2 id="ble-pair-title">{t('ble.title')}</h2>
          <button type="button" className="ble-close" onClick={onClose} aria-label={t('ble.close')}>
            ✕
          </button>
        </div>

        {!isBluetoothSupported() ? (
          <p className="ble-unsupported">{t('ble.unsupported')}</p>
        ) : (
          <>
            {step === 'intro' && (
              <div className="ble-center">
                <p className="form-hint">{t('ble.intro')}</p>
                <button type="button" className="btn-primary" onClick={findDevice}>
                  {t('ble.find')}
                </button>
              </div>
            )}

            {step === 'connecting' && <Waiting text={t('ble.connecting')} />}

            {step === 'alreadyPaired' && (
              <div className="ble-center">
                <p>{t('ble.alreadyPaired')}</p>
                <p className="form-hint">{t('ble.alreadyPairedHint')}</p>
                <button type="button" className="btn-primary" onClick={() => proceedToWifiForm(linkRef.current!)}>
                  {t('ble.continueAnyway')}
                </button>
                <button type="button" onClick={onClose}>
                  {t('ble.back')}
                </button>
              </div>
            )}

            {step === 'wifiForm' && !initialScanDone && <Waiting text={t('ble.scanningWifi')} />}

            {step === 'wifiForm' && initialScanDone && (
              <form className="ble-wifi-form" onSubmit={submitWifi}>
                <div className="ble-ssid-row">
                  <input
                    aria-label={t('ble.wifiNetwork')}
                    placeholder={t('ble.wifiNetwork')}
                    value={ssid}
                    onChange={(e) => setSsid(e.target.value)}
                    required
                  />
                  <button
                    type="button"
                    onClick={() => linkRef.current && void rescan(linkRef.current)}
                    disabled={scanningWifi}
                    aria-label={t('ble.rescan')}
                  >
                    {scanningWifi ? <span className="ble-spinner ble-spinner--small" /> : '↻'}
                  </button>
                </div>
                <input
                  ref={passwordRef}
                  type={showPassword ? 'text' : 'password'}
                  aria-label={t('ble.wifiPassword')}
                  placeholder={t('ble.wifiPassword')}
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                  autoComplete="off"
                />
                <label className="ble-show-password">
                  <input type="checkbox" checked={showPassword} onChange={(e) => setShowPassword(e.target.checked)} />
                  {t('ble.showPassword')}
                </label>
                <button type="submit" disabled={!ssid.trim()}>
                  {t('ble.connectWifi')}
                </button>
                <p className="form-hint">{t('ble.nearbyNetworks', { count: networks.length })}</p>
                {networks.length === 0 ? (
                  <p className="form-hint">{t('ble.noNetworks')}</p>
                ) : (
                  <ul className="ble-networks">
                    {networks.map((n) => (
                      <li key={n.ssid}>
                        <button
                          type="button"
                          className={n.ssid === ssid ? 'ble-network ble-network--selected' : 'ble-network'}
                          onClick={() => {
                            setSsid(n.ssid)
                            passwordRef.current?.focus()
                          }}
                        >
                          <span className="ble-network-ssid">{n.ssid}</span>
                          <span className="ble-network-rssi">{n.rssi} dBm</span>
                        </button>
                      </li>
                    ))}
                  </ul>
                )}
              </form>
            )}

            {(step === 'joining' || step === 'activating') && (
              <Waiting text={t(step === 'joining' ? 'ble.joining' : 'ble.activating')}>
                <button type="button" onClick={cancelAttempt}>
                  {t('ble.cancel')}
                </button>
              </Waiting>
            )}

            {step === 'done' && (
              <div className="ble-center">
                <span className="ble-check" aria-hidden="true">
                  ✓
                </span>
                <p>{t('ble.done')}</p>
                <button type="button" className="btn-primary" onClick={onClose}>
                  {t('ble.finish')}
                </button>
              </div>
            )}

            {step === 'error' && (
              <div className="ble-center">
                <p className="ble-error">{error ?? t('ble.genericError')}</p>
                <button type="button" className="btn-primary" onClick={retry}>
                  {t('ble.retry')}
                </button>
              </div>
            )}
          </>
        )}
      </div>
    </div>
  )
}

function Waiting({ text, children }: { text: string; children?: React.ReactNode }) {
  return (
    <div className="ble-center">
      <span className="ble-spinner" aria-hidden="true" />
      <p>{text}</p>
      {children}
    </div>
  )
}

export default BlePairing
