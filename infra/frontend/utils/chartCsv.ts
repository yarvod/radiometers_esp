import type { HistoryChartDefinition } from '~/types/charts'

const csvCell = (value: string) => {
  const safeValue = /^[=+@-]/.test(value) ? `'${value}` : value
  return `"${safeValue.replaceAll('"', '""')}"`
}

const chartValue = (value: unknown) => {
  let candidate = value
  if (Array.isArray(value)) {
    candidate = value[1]
  } else if (value && typeof value === 'object' && 'y' in value) {
    candidate = (value as { y?: unknown }).y
  }

  if (typeof candidate === 'number') {
    return Number.isFinite(candidate) ? String(candidate) : ''
  }
  if (typeof candidate === 'string' && candidate.trim() !== '' && Number.isFinite(Number(candidate))) {
    return candidate
  }
  return ''
}

export const historyChartToCsv = (definition: HistoryChartDefinition) => {
  const header = ['datetime', ...definition.datasets.map((dataset) => String(dataset.label ?? 'series'))]
  const rows = definition.datetimes.map((datetime, index) => [
    datetime,
    ...definition.datasets.map((dataset) => chartValue(Array.isArray(dataset.data) ? dataset.data[index] : undefined)),
  ])

  return [
    header.map(csvCell).join(','),
    ...rows.map(([datetime, ...values]) => [csvCell(datetime), ...values].join(',')),
  ].join('\r\n')
}

export const historyChartCsvFilename = (definition: HistoryChartDefinition) => {
  const exportedAt = new Date().toISOString().replaceAll(':', '-').replace(/\.\d{3}Z$/, 'Z')
  return `${definition.key}-${exportedAt}.csv`
}

export const downloadCsvFile = (csv: string, filename: string) => {
  const blob = new Blob([`\uFEFF${csv}`], { type: 'text/csv;charset=utf-8' })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = filename
  document.body.appendChild(link)
  link.click()
  link.remove()
  URL.revokeObjectURL(url)
}
