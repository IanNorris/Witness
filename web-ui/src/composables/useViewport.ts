import { onMounted, onUnmounted, ref } from 'vue'

// Matches the stylesheet's mobile layout breakpoints (main.scss): narrow
// screens, or coarse pointers up to small-tablet width.
const PHONE_QUERY = '(max-width: 768px), (pointer: coarse) and (max-width: 1024px)'

export function isPhoneViewport(): boolean {
  return typeof window !== 'undefined' && typeof window.matchMedia === 'function' && window.matchMedia(PHONE_QUERY).matches
}

export function useIsPhone() {
  const phone = ref(isPhoneViewport())
  let query: MediaQueryList | null = null
  const update = () => { phone.value = !!query?.matches }
  onMounted(() => {
    if (typeof window.matchMedia !== 'function') return
    query = window.matchMedia(PHONE_QUERY)
    update()
    query.addEventListener('change', update)
  })
  onUnmounted(() => query?.removeEventListener('change', update))
  return phone
}

// Size of the largest box with the given aspect ratio that fits `element`.
export function useFittedBox(element: () => HTMLElement | null, aspect: () => number) {
  const box = ref({ width: 0, height: 0 })
  let observer: ResizeObserver | null = null
  const measure = () => {
    const el = element()
    if (!el) return
    const { width, height } = el.getBoundingClientRect()
    const ratio = aspect()
    if (width <= 0 || height <= 0 || !(ratio > 0)) return
    box.value = width / height > ratio
      ? { width: Math.floor(height * ratio), height: Math.floor(height) }
      : { width: Math.floor(width), height: Math.floor(width / ratio) }
  }
  onMounted(() => {
    const el = element()
    if (!el || typeof ResizeObserver === 'undefined') return
    observer = new ResizeObserver(measure)
    observer.observe(el)
    measure()
  })
  onUnmounted(() => observer?.disconnect())
  return { box, measure }
}
