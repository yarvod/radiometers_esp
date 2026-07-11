export type HistoryChartKey = 'temp' | 'adc' | 'brightness' | 'loadCheck' | 'teff' | 'tau' | 'pwv'

export type HistoryChartDefinition = {
  key: HistoryChartKey
  title: string
  labels: string[]
  datasets: Array<Record<string, any>>
  atmosphere?: boolean
}
