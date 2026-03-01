import { ref, watch, type Ref } from 'vue'

export function useLocalStorage<T>(key: string, defaultValue: T): Ref<T> {
  let initial = defaultValue
  const stored = localStorage.getItem(key)
  if (stored !== null) {
    try {
      initial = JSON.parse(stored) as T
    } catch {
      // Plain string value (e.g. "hls") — use as-is if T is string
      initial = stored as T
    }
  }
  const data = ref<T>(initial) as Ref<T>

  watch(data, (val) => {
    localStorage.setItem(key, JSON.stringify(val))
  }, { deep: true })

  return data
}
