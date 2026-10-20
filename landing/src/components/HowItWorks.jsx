import { STEPS } from '../content.js'

export default function HowItWorks() {
  return (
    <section id="dich-vu" className="how section" aria-labelledby="how-title">
      <div className="container">
        <p className="eyebrow eyebrow--center reveal">CÁCH HOẠT ĐỘNG</p>
        <h2 id="how-title" className="section__title section__title--center reveal">
          Chăm cây chỉ với 3 bước
        </h2>
        <ol className="how__steps">
          {STEPS.map((s, i) => (
            <li key={s.title} className="step reveal" style={{ '--d': `${i * 90}ms` }}>
              <span className="step__no" aria-hidden="true">{i + 1}</span>
              <h3>{s.title}</h3>
              <p>{s.text}</p>
            </li>
          ))}
        </ol>
      </div>
    </section>
  )
}
