import { useEffect } from 'react'
import { useLocation, useNavigate } from 'react-router-dom'
import Header from '../components/Header.jsx'
import Hero from '../components/Hero.jsx'
import Story from '../components/Story.jsx'
import Product from '../components/Product.jsx'
import Team from '../components/Team.jsx'
import HowItWorks from '../components/HowItWorks.jsx'
import About from '../components/About.jsx'
import Contact from '../components/Contact.jsx'
import News from '../components/News.jsx'
import Footer from '../components/Footer.jsx'
import ScrollTop from '../components/ScrollTop.jsx'

export default function Home() {
  const navigate = useNavigate()
  const location = useLocation()
  const goToOrder = () => navigate('/dat-hang')

  useEffect(() => {
    if (!location.hash) return
    const el = document.querySelector(location.hash)
    el?.scrollIntoView({ behavior: 'smooth' })
  }, [location.hash])

  return (
    <>
      <Header onOrder={goToOrder} />
      <main id="main">
        <Hero onOrder={goToOrder} />
        <Story />
        <HowItWorks />
        <Product onOrder={goToOrder} />
        <Team />
        <About />
        <Contact />
        <News />
      </main>
      <Footer />
      <ScrollTop />
    </>
  )
}
