import { useEffect, useMemo, useRef, useState } from 'react'
import {
  UsbFlasher,
  downloadAppImage,
  downloadKitFile,
  errorText,
  fetchManifest,
  webSerialSupported,
  type FlashImage,
} from './usbflash/flasher'

export type UsbBuild = { id: string; version: string; sha256: string; hasFlashKit: boolean }

type Mode = 'update' | 'full'
type Status = { kind: 'ok' | 'error' | 'info'; text: string } | null
type Progress = { label: string; file: number; overall: number }

function UsbFlash({ builds }: { builds: UsbBuild[] }) {
  const supported = useMemo(webSerialSupported, [])
  const [buildId, setBuildId] = useState('')
  const [mode, setMode] = useState<Mode>('update')
  const [eraseAll, setEraseAll] = useState(false)
  const [chip, setChip] = useState<string | null>(null)
  const [busy, setBusy] = useState<'connecting' | 'flashing' | null>(null)
  const [progress, setProgress] = useState<Progress | null>(null)
  const [status, setStatus] = useState<Status>(null)
  const [log, setLog] = useState<string[]>([])
  const [showLog, setShowLog] = useState(false)
  const flasher = useRef<UsbFlasher | null>(null)

  const build = builds.find((b) => b.id === buildId) ?? builds[0]
  const fullAllowed = !!build?.hasFlashKit

  const appendLog = (line: string) => setLog((lines) => [...lines.slice(-300), line])

  useEffect(() => {
    if (!fullAllowed && mode === 'full') setMode('update')
  }, [fullAllowed, mode])

  useEffect(() => () => void flasher.current?.disconnect(), [])

  if (!supported) {
    return (
      <div className="card fw-usb">
        <h3>Flash via USB</h3>
        <p className="form-hint">
          USB flashing needs Chrome or Edge on a desktop computer (Web Serial), over https or localhost. Phones and
          Safari/Firefox can't do it -- use OTA updates below instead.
        </p>
      </div>
    )
  }

  const connect = async () => {
    setStatus(null)
    setBusy('connecting')
    const instance = new UsbFlasher(appendLog)
    try {
      const description = await instance.connect()
      flasher.current = instance
      setChip(description)
      setStatus({ kind: 'ok', text: `Connected: ${description}` })
    } catch (err) {
      // Closing the browser's port picker is a deliberate cancel, not an error.
      if (!(err instanceof DOMException && err.name === 'NotFoundError')) {
        setStatus({ kind: 'error', text: errorText(err) })
      }
    } finally {
      setBusy(null)
    }
  }

  const disconnect = async () => {
    await flasher.current?.disconnect()
    flasher.current = null
    setChip(null)
  }

  const prepareImages = async (): Promise<{ images: FlashImage[]; settings: { flashMode: string; flashFreq: string; flashSize: string } }> => {
    const manifest = build.hasFlashKit ? await fetchManifest(build.id) : null
    const settings = {
      flashMode: manifest?.flashMode ?? 'dio',
      flashFreq: manifest?.flashFreq ?? '80m',
      flashSize: manifest?.flashSize ?? '16MB',
    }
    if (mode === 'full') {
      if (!manifest) throw new Error('This build has no flash kit -- upload the flash-kit .zip to enable full flash.')
      const images: FlashImage[] = []
      for (const file of manifest.files) {
        appendLog(`Downloading ${file.name}…`)
        images.push({ name: file.name, offset: file.offset, data: await downloadKitFile(build.id, file) })
      }
      return { images, settings }
    }

    const kitApp = manifest?.files.find((f) => f.role === 'app')
    const kitOtadata = manifest?.files.find((f) => f.role === 'otadata')
    appendLog('Downloading app image…')
    const app = kitApp ? await downloadKitFile(build.id, kitApp) : await downloadAppImage(build.id, build.sha256)
    const otadata = kitOtadata ? await downloadKitFile(build.id, kitOtadata) : null
    const images = await flasher.current!.planUpdate(app, otadata, manifest)
    return { images, settings }
  }

  const flash = async () => {
    if (!flasher.current || !build) return
    const warning =
      mode === 'full'
        ? eraseAll
          ? `Erase the ENTIRE flash and write ${build.version}? This wipes Wi-Fi settings and pairing -- the board must be set up again.`
          : `Write bootloader, partition table and ${build.version} to the board?`
        : `Update the board to ${build.version}? Wi-Fi settings and pairing are kept.`
    if (!window.confirm(warning)) return

    setStatus(null)
    setBusy('flashing')
    setProgress({ label: 'Preparing', file: 0, overall: 0 })
    try {
      const { images, settings } = await prepareImages()
      const sizes = images.map((img) => img.data.length)
      const total = sizes.reduce((a, b) => a + b, 0)
      await flasher.current.flash(images, { ...settings, eraseAll: mode === 'full' && eraseAll }, (index, written, fileTotal) => {
        const fraction = fileTotal > 0 ? written / fileTotal : 1
        const done = sizes.slice(0, index).reduce((a, b) => a + b, 0) + fraction * sizes[index]
        setProgress({
          label: images[index].name,
          file: Math.round(fraction * 100),
          overall: Math.round((done / total) * 100),
        })
      })
      setProgress({ label: 'Done', file: 100, overall: 100 })
      setStatus({ kind: 'ok', text: `Flashed ${build.version}. The board is rebooting -- unplug it when the screen comes up.` })
      await disconnect()
    } catch (err) {
      setStatus({ kind: 'error', text: `Flash failed: ${errorText(err)}` })
      appendLog(`ERROR: ${errorText(err)}`)
      setShowLog(true)
      await disconnect()
    } finally {
      setBusy(null)
    }
  }

  return (
    <div className="card fw-usb">
      <h3>Flash via USB</h3>
      <p className="form-hint">
        Plug the board into this computer with a USB cable (either USB-C port on the dev kit, "USB" preferred) — no
        button needed, it enters flash mode automatically. Only if connecting fails (e.g. a crash-looping board), hold
        BOOT while plugging it in.
      </p>
      {builds.length === 0 ? (
        <p className="form-hint">Upload a build first.</p>
      ) : (
        <>
          <select aria-label="Build to flash" value={build?.id ?? ''} onChange={(e) => setBuildId(e.target.value)} disabled={!!busy}>
            {builds.map((b, i) => (
              <option key={b.id} value={b.id}>
                {b.version}
                {i === 0 ? ' (latest)' : ''}
                {b.hasFlashKit ? ' -- USB kit' : ''}
              </option>
            ))}
          </select>
          <fieldset className="fw-usb-modes" disabled={!!busy}>
            <label>
              <input type="radio" name="usb-mode" checked={mode === 'update'} onChange={() => setMode('update')} />
              <span>
                <strong>Update firmware</strong> -- keeps Wi-Fi &amp; pairing
              </span>
            </label>
            <label title={fullAllowed ? undefined : 'Needs a build uploaded as a flash-kit .zip'}>
              <input type="radio" name="usb-mode" checked={mode === 'full'} disabled={!fullAllowed} onChange={() => setMode('full')} />
              <span>
                <strong>Full flash</strong> -- new or bricked board
                {!fullAllowed && <em className="form-hint"> (needs a flash-kit upload)</em>}
              </span>
            </label>
            {mode === 'full' && (
              <label className="fw-usb-erase">
                <input type="checkbox" checked={eraseAll} onChange={(e) => setEraseAll(e.target.checked)} />
                <span>Erase entire flash first -- wipes Wi-Fi settings and pairing</span>
              </label>
            )}
          </fieldset>
          <div className="fw-usb-actions">
            {chip ? (
              <>
                <button type="button" className="btn-primary" onClick={flash} disabled={!!busy}>
                  {busy === 'flashing' ? 'Flashing…' : `Flash ${build?.version}`}
                </button>
                <button type="button" onClick={disconnect} disabled={!!busy}>
                  Disconnect
                </button>
              </>
            ) : (
              <button type="button" className="btn-primary" onClick={connect} disabled={!!busy}>
                {busy === 'connecting' ? 'Connecting…' : 'Connect board'}
              </button>
            )}
          </div>
        </>
      )}
      {progress && (
        <div className="fw-usb-progress">
          <span className="form-hint">
            {progress.label} -- {progress.file}% (overall {progress.overall}%)
          </span>
          <div className="fw-progress" role="progressbar" aria-valuenow={progress.overall} aria-valuemin={0} aria-valuemax={100}>
            <div style={{ width: `${progress.overall}%` }} />
          </div>
        </div>
      )}
      {status && <p className={status.kind === 'error' ? 'form-error' : 'form-status'}>{status.text}</p>}
      {log.length > 0 && (
        <details className="fw-usb-log" open={showLog} onToggle={(e) => setShowLog(e.currentTarget.open)}>
          <summary>Log ({log.length} lines)</summary>
          <pre>{log.join('\n')}</pre>
        </details>
      )}
    </div>
  )
}

export default UsbFlash
