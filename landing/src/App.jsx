import { useEffect } from 'react'
import { Navigate, Route, Routes } from 'react-router-dom'
import Home from './pages/Home.jsx'
import NewsPage from './pages/NewsPage.jsx'
import OrderPage from './pages/OrderPage.jsx'
import PostPage from './pages/PostPage.jsx'

export default function App() {
  return (
    <Routes>
      <Route path="/" element={<Home />} />
      <Route path="/dat-hang" element={<OrderPage />} />
      <Route path="/tin-tuc" element={<NewsPage />} />
      <Route path="/tin-tuc/:slug" element={<PostPage />} />
      <Route path="/login" element={<ConsoleRedirect />} />
      <Route path="/download" element={<ExternalRedirect to="/downloads/" />} />
      {/* Unknown paths (e.g. old /admin links) go home instead of rendering an empty page. */}
      <Route path="*" element={<Navigate to="/" replace />} />
    </Routes>
  )
}

// nginx already 302s /login -> /app/ and /download -> /downloads/; these cover
// in-app navigation and the Vite dev server, where those nginx rules aren't in front.
function ExternalRedirect({ to }) {
  useEffect(() => {
    window.location.replace(to)
  }, [to])
  return null
}

function ConsoleRedirect() {
  return <ExternalRedirect to="/app/" />
}
