import { FEATURES } from '../content.js'

export default function About() {
  return (
    <section id="gioi-thieu" className="about section" aria-labelledby="about-title">
      <div className="container">

        <div className="about__grid">
          <div className="about__logo reveal">
            <img src="/img/logo.webp" width="512" height="512" alt="Logo SYNORA" loading="lazy" decoding="async" />
          </div>
          <div>
            <p className="eyebrow reveal">GIỚI THIỆU</p>
            <h2 id="about-title" className="section__title reveal">
              Giải pháp chăm sóc cây thông minh cho không gian sống hiện đại
            </h2>
            <p className="reveal">
              Synora là hệ thống chậu cây thông minh tích hợp công nghệ IoT, được thiết kế để mang công nghệ
              vào không gian sống xanh của bạn. Chúng tôi giúp những người bận rộn chăm sóc cây tự động, hiệu
              quả và tiện lợi, giải quyết triệt để nỗi lo quên tưới nước hay thiếu kinh nghiệm làm vườn.
            </p>
            <div className="features">
              {FEATURES.map((f, i) => (
                <article key={f.no} className="feature reveal" style={{ '--d': `${i * 90}ms` }}>
                  <span className="feature__no" aria-hidden="true">{f.no}</span>
                  <div>
                    <h3 className="feature__title">{f.title}</h3>
                    <ul>
                      {f.items.map((it) => (
                        <li key={it}>{it}</li>
                      ))}
                    </ul>
                  </div>
                </article>
              ))}
            </div>
          </div>
        </div>
      </div>
    </section>
  )
}
