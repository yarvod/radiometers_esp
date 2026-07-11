export type DeviceTab = 'data' | 'control' | 'gps' | 'meteo' | 'calibration' | 'settings' | 'errors'

export type DeviceConfig = {
  id: string
  display_name: string | null
  created_at: string
  last_seen_at: string | null
  temp_labels: string[]
  temp_addresses: string[]
  temp_label_map: Record<string, string>
  temp_bindings: Record<string, string>
  atmosphere_config: AtmosphereConfig
  adc_labels: Record<string, string>
  has_meteo: boolean
}

export type AtmosphereBandKey = 'adc2' | 'adc3'

export type AtmosphereBandConfig = {
  mode?: string
  alpha?: number | null
  beta?: number | null
}

export type AtmosphereConfig = {
  station_ids?: string[]
  altitude_m?: number
  h0_m?: number
  tau_station_id?: string
  tau_average?: boolean
  season?: string
  bands?: Partial<Record<'adc1' | AtmosphereBandKey, AtmosphereBandConfig>>
}

export type DeviceGpsConfig = {
  device_id: string
  has_gps: boolean
  rtcm_types: number[]
  mode: string
  actual_mode: string | null
  updated_at: string | null
  created_at: string | null
}

export type GnssData = {
  id: string
  device_id: string
  name: string
  description: string | null
  measurement_count: number
  start_at: string | null
  end_at: string | null
  last_import_at: string | null
  created_at: string
  updated_at: string
}

export type GnssDataMeasurementPoint = {
  measured_at: string
  timestamp_ms: number | null
  pw_mm: number
  spw_mm: number | null
  temperature_c: number | null
}

export type GnssDataSeries = {
  dataset: GnssData
  points: GnssDataMeasurementPoint[]
  capped: boolean
}

export type GnssDataSeriesResponse = {
  items: GnssDataSeries[]
  limit_per_dataset: number
}

export type GnssDataImportResponse = {
  dataset: GnssData
  parsed_rows: number
  upserted_rows: number
  duplicate_rows: number
  skipped_rows: number
  first_timestamp: string | null
  last_timestamp: string | null
  errors: string[]
}

export type ErrorEvent = {
  id: string
  device_id: string
  timestamp: string
  timestamp_ms: number | null
  code: string
  severity: string
  message: string
  active: boolean
  created_at: string
}

export type ErrorEventsResponse = {
  items: ErrorEvent[]
  total: number
  limit: number
  offset: number
}

export type MeteoHistoryPoint = {
  timestamp: string
  timestamp_ms: number
  temp_c: number | null
  humidity_pct: number | null
  wind_speed_ms: number | null
  gust_speed_ms: number | null
  wind_dir_deg: number | null
  pressure_hpa: number | null
  rainfall_mm: number | null
  light_lux: number | null
  uvi: number | null
}

export type MeteoHistoryResponse = {
  points: MeteoHistoryPoint[]
  raw_count: number
  limit: number
  bucket_seconds: number
  bucket_label: string
  aggregated: boolean
}
