import { useEffect, useState } from 'react'
import { apiFetch } from './api'

type DuplicateDevice = {
  id: string
  macAddress: string | null
  name: string
  ownerUserId: string | null
  ownerEmail: string | null
  createdAt: string
  pairedAt: string | null
  hasRelatedData: boolean
}

type DuplicateGroup = {
  normalizedMacAddress: string
  devices: DuplicateDevice[]
}

type Status = { type: 'success' | 'error'; message: string } | null

// Admin-only cleanup view for the case-mismatched-MAC duplicate device rows
// that NormalizeMac() (server's Program.cs) stops from being created going
// forward but can't retroactively fix -- lets an admin see every group of
// devices that share the same MAC (normalized) and hard-delete the dead
// leftover row(s) via the new DELETE /admin/devices/{id} endpoint, instead
// of that needing a manual DB query every time.
function DuplicateDevices() {
  const [groups, setGroups] = useState<DuplicateGroup[] | null>(null)
  const [loadError, setLoadError] = useState<string | null>(null)
  const [status, setStatus] = useState<Status>(null)
  const [deletingId, setDeletingId] = useState<string | null>(null)

  const load = () => {
    setLoadError(null)
    apiFetch('/admin/duplicate-devices')
      .then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        return res.json()
      })
      .then(setGroups)
      .catch((err) => setLoadError(`Could not load duplicate devices (${err}).`))
  }

  useEffect(load, [])

  const handleDelete = async (device: DuplicateDevice) => {
    const confirmed = window.confirm(
      `Permanently delete "${device.name}" (${device.id})?\n\nThis removes the row from the database entirely and ` +
        'cannot be undone -- it is not the same as unpairing.',
    )
    if (!confirmed) return

    setStatus(null)
    setDeletingId(device.id)
    const res = await apiFetch(`/admin/devices/${device.id}`, { method: 'DELETE' })

    if (res.ok) {
      setStatus({ type: 'success', message: `Deleted "${device.name}" (${device.id}).` })
      load()
    } else {
      let detail = `HTTP ${res.status}`
      try {
        const body = await res.json()
        if (body?.detail) detail = body.detail
      } catch {
        // response wasn't JSON -- fall back to the plain status above
      }
      setStatus({ type: 'error', message: `Could not delete "${device.name}": ${detail}` })
    }
    setDeletingId(null)
  }

  return (
    <section className="duplicate-devices">
      <h2>Duplicate devices</h2>
      <p className="admin-test-hint">
        Devices whose MAC address matches another device once both are compared case-insensitively -- the kind of
        leftover row a case mismatch could create before activation started normalizing MACs. Review each group and
        hard-delete whichever row is the dead duplicate.
      </p>

      {loadError && <p className="form-error">{loadError}</p>}
      {status && (
        <p className={status.type === 'success' ? 'form-status' : 'form-error'}>{status.message}</p>
      )}

      {groups === null && !loadError && <p>Loading…</p>}
      {groups !== null && groups.length === 0 && <p>No duplicate devices found.</p>}

      {groups !== null && groups.length > 0 && (
        <ul className="duplicate-groups">
          {groups.map((group) => (
            <li key={group.normalizedMacAddress} className="duplicate-group">
              <h3 className="device-mac">{group.normalizedMacAddress}</h3>
              <div className="table-scroll">
              <table>
                <thead>
                  <tr>
                    <th>Name</th>
                    <th>Raw MAC</th>
                    <th>Owner</th>
                    <th>Created</th>
                    <th>Paired</th>
                    <th>Related data</th>
                    <th></th>
                  </tr>
                </thead>
                <tbody>
                  {group.devices.map((d) => (
                    <tr key={d.id}>
                      <td>{d.name}</td>
                      <td className="device-mac">{d.macAddress}</td>
                      <td>{d.ownerEmail ?? (d.ownerUserId ? d.ownerUserId : 'unpaired')}</td>
                      <td>{new Date(d.createdAt).toLocaleString()}</td>
                      <td>{d.pairedAt ? new Date(d.pairedAt).toLocaleString() : '—'}</td>
                      <td>
                        {d.hasRelatedData ? (
                          <span title="Has SensorReadings and/or WateringSchedules -- hard delete will be refused.">
                            Yes
                          </span>
                        ) : (
                          <span>No</span>
                        )}
                      </td>
                      <td>
                        <button
                          type="button"
                          className="btn-danger"
                          onClick={() => handleDelete(d)}
                          disabled={deletingId === d.id}
                          title={
                            d.hasRelatedData
                              ? 'This device has sensor readings or watering schedules -- the server will refuse to hard-delete it.'
                              : 'Permanently remove this row.'
                          }
                        >
                          {deletingId === d.id ? 'Deleting…' : 'Hard delete'}
                        </button>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
              </div>
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}

export default DuplicateDevices
