export const parseDateAny = (value: string | number | Date | null | undefined) => {
  if (value instanceof Date) return value
  if (value === null || value === undefined || value === '') return null
  const date = new Date(value)
  return Number.isNaN(date.getTime()) ? null : date
}

export const toLocalInputValue = (date: Date) => {
  const pad = (value: number) => String(value).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`
}

export const localInputToIso = (value: string) => {
  if (!value) return ''
  const date = new Date(value)
  return Number.isNaN(date.getTime()) ? '' : date.toISOString()
}

export const formatLocalDateTime = (value: string | number | Date | null | undefined) => {
  const date = parseDateAny(value)
  return date ? date.toLocaleString() : '—'
}

export const formatChartTimestamp = (value: string | number | Date) => {
  const date = parseDateAny(value)
  return date ? date.toLocaleString() : String(value)
}

export const browserTimezoneLabel = () => {
  if (!process.client) return ''
  const tz = Intl.DateTimeFormat().resolvedOptions().timeZone || 'local'
  const offsetMin = new Date().getTimezoneOffset()
  const sign = offsetMin <= 0 ? '+' : '-'
  const abs = Math.abs(offsetMin)
  const hh = String(Math.floor(abs / 60)).padStart(2, '0')
  const mm = String(abs % 60).padStart(2, '0')
  return `${tz} (UTC${sign}${hh}:${mm})`
}
