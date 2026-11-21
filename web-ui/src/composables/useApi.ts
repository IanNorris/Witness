import { ref } from 'vue'

const csrfToken = ref('')

export function setCsrfToken(token: string) {
  csrfToken.value = token
}

// Carries the HTTP status and parsed JSON body so callers can distinguish
// e.g. a revision conflict (409) from a validation failure (422).
export class ApiError extends Error {
  readonly status: number
  readonly body: unknown
  constructor(message: string, status: number, body: unknown) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.body = body
  }
}

export interface ApiOptions {
  method?: 'GET' | 'POST' | 'PUT' | 'DELETE'
  body?: Record<string, unknown>
  redirectOnFail?: boolean
  signal?: AbortSignal
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
    signal: options.signal,
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
    // Try to include server error message
    let errorMsg = `API error: ${response.status} ${response.statusText}`
    let errBody: unknown = null
    try {
      errBody = await response.json()
      const error = (errBody as { error?: unknown } | null)?.error
      if (typeof error === 'string' && error) errorMsg = error
    } catch { /* ignore parse errors */ }
    throw new ApiError(errorMsg, response.status, errBody)
  }

  const text = await response.text()
  if (!text) return undefined as T

  try {
    return JSON.parse(text) as T
  } catch {
    return text as T
  }
}
