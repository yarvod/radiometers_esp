export type HistoryChartKey = 'temp' | 'adc' | 'brightness' | 'loadCheck' | 'teff' | 'tau' | 'pwv' | 'meteo'

export type HistoryChartDefinition = {
  key: HistoryChartKey
  title: string
  labels: string[]
  datetimes: string[]
  datasets: Array<Record<string, any>>
  atmosphere?: boolean
}
