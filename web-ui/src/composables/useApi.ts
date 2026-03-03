import { ref } from 'vue'

const csrfToken = ref('')

export function setCsrfToken(token: string) {
  csrfToken.value = token
}

export interface ApiOptions {
  method?: 'GET' | 'POST' | 'PUT' | 'DELETE'
  body?: Record<string, unknown>
  redirectOnFail?: boolean
}

export async function api<T = unknown>(
  url: string,
  options: ApiOptions = {},
): Promise<T> {
  const { method = 'GET', body, redirectOnFail = true } = options

  const fetchOptions: RequestInit = {
    method,
    credentials: 'same-origin',
    headers: {} as Record<string, string>,
  }

  if (body || method === 'POST') {
    const payload = { csrf: csrfToken.value, ...body }
    fetchOptions.headers = { 'Content-Type': 'application/json' }
    fetchOptions.body = JSON.stringify(payload)
  }

  const response = await fetch(url, fetchOptions)

  if (!response.ok) {
    if ((response.status === 401 || response.status === 403) && redirectOnFail) {
      if (window.location.pathname !== '/login') {
        window.location.replace('/login')
      }
      throw new Error('Unauthorized')
    }
    throw new Error(`API error: ${response.status} ${response.statusText}`)
  }

  const text = await response.text()
  if (!text) return undefined as T

  try {
    return JSON.parse(text) as T
  } catch {
    return text as T
  }
}
