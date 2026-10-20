// Fade-in on scroll for every `.reveal` element, including ones mounted later.
export function initReveal() {
  if (!('IntersectionObserver' in window)) {
    document.documentElement.classList.add('no-reveal')
    return
  }
  const io = new IntersectionObserver(
    (entries) => {
      for (const e of entries) {
        if (e.isIntersecting) {
          e.target.classList.add('is-in')
          io.unobserve(e.target)
        }
      }
    },
    { threshold: 0.12 },
  )
  const scan = () => document.querySelectorAll('.reveal:not(.is-in)').forEach((el) => io.observe(el))
  requestAnimationFrame(scan)
  new MutationObserver(scan).observe(document.getElementById('root'), { childList: true, subtree: true })
}
