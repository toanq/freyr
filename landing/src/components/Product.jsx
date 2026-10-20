import { LeafIcon } from './Icons.jsx'

export default function Product({ onOrder }) {
  return (
    <section className="product" aria-labelledby="product-title">
      <div className="container product__inner reveal">
        <span className="product__icon"><LeafIcon width={28} height={28} /></span>
        <p className="eyebrow eyebrow--light">SẢN PHẨM ĐẦU TIÊN</p>
        <h2 id="product-title" className="product__title">SYNORA Smart Planter</h2>
        <p className="product__lead">
          Đây là bước đi đầu tiên trong hành trình mang công nghệ xanh vào đời sống của chúng tôi. Một chiếc
          chậu cây không chỉ đẹp mà còn cực kỳ thông minh.
        </p>
        <button type="button" className="btn btn--light btn--lg" onClick={onOrder}>
          Đặt hàng ngay
        </button>
      </div>
    </section>
  )
}
