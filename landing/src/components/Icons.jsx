const base = { width: 20, height: 20, viewBox: '0 0 24 24', fill: 'none', stroke: 'currentColor', strokeWidth: 1.8, strokeLinecap: 'round', strokeLinejoin: 'round', 'aria-hidden': true }

export const PinIcon = (p) => (
  <svg {...base} {...p}><path d="M12 22s7-6.2 7-12a7 7 0 0 0-14 0c0 5.8 7 12 7 12z" /><circle cx="12" cy="10" r="2.5" /></svg>
)
export const MailIcon = (p) => (
  <svg {...base} {...p}><rect x="3" y="5" width="18" height="14" rx="2" /><path d="m3 7 9 6 9-6" /></svg>
)
export const PhoneIcon = (p) => (
  <svg {...base} {...p}><path d="M5 4h4l2 5-2.5 1.5a11 11 0 0 0 5 5L15 13l5 2v4a2 2 0 0 1-2 2A16 16 0 0 1 3 6a2 2 0 0 1 2-2z" /></svg>
)
export const MenuIcon = (p) => (
  <svg {...base} {...p}><path d="M4 6h16M4 12h16M4 18h16" /></svg>
)
export const CloseIcon = (p) => (
  <svg {...base} {...p}><path d="M6 6l12 12M18 6 6 18" /></svg>
)
export const ArrowUpIcon = (p) => (
  <svg {...base} {...p}><path d="M12 19V5M5 12l7-7 7 7" /></svg>
)
export const LeafIcon = (p) => (
  <svg {...base} {...p}><path d="M5 19c0-8 5-14 15-15-1 10-7 15-15 15z" /><path d="M5 19 14 10" /></svg>
)
export const CalendarIcon = (p) => (
  <svg {...base} {...p}><rect x="3" y="5" width="18" height="16" rx="2" /><path d="M16 3v4M8 3v4M3 10h18" /></svg>
)
export const FacebookIcon = (p) => (
  <svg {...base} fill="currentColor" stroke="none" {...p}><path d="M14 8h3V4h-3c-2.8 0-4 1.7-4 4.3V11H7v4h3v7h4v-7h3l1-4h-4V8.6c0-.4.3-.6 1-.6z" /></svg>
)
export const TiktokIcon = (p) => (
  <svg {...base} fill="currentColor" stroke="none" {...p}><path d="M16.5 3c.4 2.3 1.8 3.8 4 4v3.3a7.6 7.6 0 0 1-4-1.2v6.4a5.8 5.8 0 1 1-5.8-5.8c.3 0 .6 0 .9.1v3.4a2.5 2.5 0 1 0 1.6 2.3V3h3.3z" /></svg>
)
export const CheckIcon = (p) => (
  <svg {...base} {...p}><path d="m5 12 5 5L20 7" /></svg>
)
