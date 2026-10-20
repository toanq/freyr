import { HIGHLIGHTS } from '../content.js'
import { CheckIcon } from './Icons.jsx'

export default function Hero({ onOrder }) {
  return (
    <section className="hero" aria-labelledby="hero-title">
      <div className="container hero__inner">
        <p className="eyebrow reveal">ĐƯA THIÊN NHIÊN VÀO PHỐ THỊ</p>
        <h1 id="hero-title" className="hero__title reveal">
          Để Cây Xanh <span>Tự Chăm Sóc</span> Theo Cách Thông Minh Nhất
        </h1>
        <p className="hero__lead reveal">
          Chúng tôi tạo ra giải pháp giúp bạn mang mảng xanh vào nhà một cách dễ dàng. Không còn nỗi lo
          quên tưới nước hay thiếu nắng, SYNORA giúp cây của bạn luôn tràn đầy sức sống.
        </p>
        <div className="hero__ctas reveal">
          <button type="button" className="btn btn--primary btn--lg" onClick={onOrder}>
            Đặt hàng ngay
          </button>
          <a href="#cau-chuyen" className="btn btn--ghost btn--lg">
            Tìm hiểu hành trình của chúng tôi
          </a>
        </div>
        <ul className="hero__chips reveal" aria-label="Tính năng nổi bật">
          {HIGHLIGHTS.map((h) => (
            <li key={h}>
              <CheckIcon width={16} height={16} /> {h}
            </li>
          ))}
        </ul>
      </div>
    </section>
  )
}
