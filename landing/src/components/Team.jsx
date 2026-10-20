import { TEAM } from '../content.js'

export default function Team() {
  return (
    <section className="team section" aria-labelledby="team-title">
      <div className="container">
        <p className="eyebrow eyebrow--center reveal">ĐỘI NGŨ</p>
        <h2 id="team-title" className="section__title section__title--center reveal">Chúng tôi là ai?</h2>
        <figure className="team__frame reveal">
          <img
            src="/img/team.webp"
            width="1920"
            height="1080"
            loading="lazy"
            decoding="async"
            alt="Ảnh đội ngũ sáng lập SYNORA gồm sáu thành viên"
          />
        </figure>
        {/* The photo's captions are unreadable on phones and invisible to screen readers. */}
        <ul className="team__list">
          {TEAM.map((m, i) => (
            <li key={m.name} className="member reveal" style={{ '--d': `${i * 60}ms` }}>
              <span className="member__role">{m.role}</span>
              <h3>{m.name}</h3>
              <p>{m.duty}</p>
            </li>
          ))}
        </ul>
      </div>
    </section>
  )
}
