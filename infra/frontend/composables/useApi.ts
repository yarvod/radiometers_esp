export const useApi = () => {
  const config = useRuntimeConfig()
  const token = useCookie('access_token')

  const apiFetch = async <T>(path: string, options: any = {}): Promise<T> => {
    const headers: Record<string, string> = { ...(options.headers || {}) }
    if (token.value) {
      headers.Authorization = `Bearer ${token.value}`
    }
    try {
      return await $fetch<T>(`${config.public.apiBase}${path}`, {
        ...options,
        headers,
        credentials: 'include',
      })
    } catch (err: any) {
      const status = err?.response?.status
      if (status === 401) {
        token.value = null
        if (process.client) {
          await navigateTo('/login')
        }
      }
      throw err
    }
  }

  return { apiFetch, token }
}
