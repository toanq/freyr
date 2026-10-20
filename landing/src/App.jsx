import { Route, Routes } from 'react-router-dom'
import AdminApp from './pages/admin/AdminApp.jsx'
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
      <Route path="/admin/*" element={<AdminApp />} />
    </Routes>
  )
}
