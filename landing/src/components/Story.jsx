export default function Story() {
  return (
    <section id="cau-chuyen" className="story section" aria-labelledby="story-title">
      <div className="container story__grid">
        <div className="story__media reveal">
          <img
            src="/img/story-plant.webp"
            width="1100"
            height="647"
            loading="lazy"
            decoding="async"
            alt="Cây non với gân lá phát sáng như mạch điện tử, tượng trưng cho thiên nhiên kết hợp công nghệ"
          />
        </div>
        <div className="story__body">
          <p className="eyebrow reveal">CÂU CHUYỆN CỦA CHÚNG TÔI</p>
          <h2 id="story-title" className="section__title reveal">
            Khởi nguồn từ tình yêu với những khoảng <em>xanh</em>
          </h2>
          <p className="reveal">
            SYNORA bắt đầu từ một niềm trăn trở đơn giản: làm sao để những người bận rộn giữa lòng thành
            phố vẫn có thể duy trì một góc vườn xanh tươi ngay trong nhà mình.
          </p>
          <p className="reveal">
            Chúng tôi đã chứng kiến quá nhiều chậu cây héo úa chỉ vì thiếu đi sự thấu hiểu. Chính vì vậy,
            đội ngũ SYNORA đã nghiên cứu cách kết hợp giữa công nghệ hiện đại và nhu cầu tự nhiên của thực
            vật, tạo nên một "ngôn ngữ chung" giữa con người và cây xanh.
          </p>
          <blockquote className="story__quote reveal">
            "Mục tiêu của chúng tôi không chỉ là bán một chậu cây, mà là lan tỏa một lối sống bền vững, nơi
            công nghệ phục vụ cho sự sống."
          </blockquote>
        </div>
      </div>
    </section>
  )
}
