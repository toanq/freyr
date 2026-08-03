import { useEffect, useState } from 'react'
import { apiFetch, API_URL } from './api'

type HealthState =
  | { status: 'loading' }
  | { status: 'ok' }
  | { status: 'error'; message: string }

function Dashboard() {
  const [health, setHealth] = useState<HealthState>({ status: 'loading' })

  useEffect(() => {
    apiFetch('/health')
      .then((res) => {
        if (!res.ok) throw new Error(`HTTP ${res.status}`)
        return res.json()
      })
      .then(() => setHealth({ status: 'ok' }))
      .catch((err) => setHealth({ status: 'error', message: String(err) }))
  }, [])

  return (
    <section className="dashboard">
      <h2>Server status</h2>
      <p>Server: {API_URL}</p>
      <p className={`health health--${health.status}`}>
        {health.status === 'loading' && 'Checking server…'}
        {health.status === 'ok' && 'Server is up'}
        {health.status === 'error' && `Server unreachable (${health.message})`}
      </p>
    </section>
  )
}

export default Dashboard
