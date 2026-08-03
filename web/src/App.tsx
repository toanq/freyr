import { useEffect, useState } from 'react'
import { apiFetch } from './api'
import Login from './Login'
import Shell from './Shell'
import './App.css'

type Session = { username: string; role: string }
type AuthState =
  | { status: 'checking' }
  | { status: 'anonymous' }
  | { status: 'authenticated'; session: Session }

function App() {
  const [auth, setAuth] = useState<AuthState>({ status: 'checking' })

  const checkSession = () => {
    apiFetch('/auth/session')
      .then((res) => res.json())
      .then((data) =>
        setAuth(
          data.authenticated
            ? { status: 'authenticated', session: { username: data.username, role: data.role } }
            : { status: 'anonymous' },
        ),
      )
      .catch(() => setAuth({ status: 'anonymous' }))
  }

  useEffect(checkSession, [])

  if (auth.status === 'checking') return null
  if (auth.status === 'anonymous') return <Login onLoggedIn={checkSession} />
  return <Shell session={auth.session} onLoggedOut={() => setAuth({ status: 'anonymous' })} />
}

export default App
