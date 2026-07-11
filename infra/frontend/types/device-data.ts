import type { AtmosphereConfig } from './device'

export type MeasurementPoint = {
  timestamp: string
  timestamp_ms: number | null
  adc1: number
  adc2: number
  adc3: number
  temps: number[]
  bus_v: number
  bus_i: number
  bus_p: number
  adc1_cal: number | null
  adc2_cal: number | null
  adc3_cal: number | null
  gps_lat: number | null
  gps_lon: number | null
  gps_alt: number | null
  gps_fix_quality: number | null
  gps_satellites: number | null
  gps_fix_age_ms: number | null
  brightness_temp1: number | null
  brightness_temp2: number | null
  brightness_temp3: number | null
  cal_brightness_temp1: number | null
  cal_brightness_temp2: number | null
  cal_brightness_temp3: number | null
}

export type TempOutlierFilterStats = {
  enabled: boolean
  window: number
  threshold: number
  min_count: number
  inspected_indices: number[]
  removed_count: number
  input_count: number
  output_count: number
}

export type MeasurementsResponse = {
  points: MeasurementPoint[]
  raw_count: number
  limit: number
  bucket_seconds: number
  bucket_label: string
  aggregated: boolean
  temp_labels: string[]
  temp_label_map: Record<string, string>
  adc_labels: Record<string, string>
  temp_addresses: string[]
  temp_bindings: Record<string, string>
  brightness_temp_labels: Record<string, string>
  temp_outlier_filter?: TempOutlierFilterStats
}

export type AtmosphereCoefficientPair = { alpha: number | null; beta: number | null }
export type AtmosphereCoefficientRow = {
  height_km: number
  summer: AtmosphereCoefficientPair
  winter: AtmosphereCoefficientPair
}
export type AtmosphereCoefficientDefaults = {
  altitude_m: number
  season: string
  effective_season: 'summer' | 'winter'
  table: Record<string, AtmosphereCoefficientRow[]>
  channels: Record<string, {
    label: string
    mode: string
    band: string
    season: 'summer' | 'winter'
    manual: AtmosphereCoefficientPair
    table: AtmosphereCoefficientPair
    effective: AtmosphereCoefficientPair
    seasons: Record<'summer' | 'winter', AtmosphereCoefficientPair>
  }>
}

export type StationOption = { station_id: string; name: string | null }
export type AtmosphereTeffPoint = {
  station_id: string
  station_name: string | null
  sounding_time: string
  t_eff: number | null
  pwv_profile: number | null
  row_count: number
}
export type AtmosphereMeasurementPoint = {
  timestamp: string
  timestamp_ms: number | null
  t_eff: number | null
  t_eff_station_id: string | null
  t_eff_age_hours: number | null
  brightness_temp1: number | null
  brightness_temp2: number | null
  brightness_temp3: number | null
  tau1: number | null
  tau2: number | null
  tau3: number | null
  alpha1: number | null
  alpha2: number | null
  alpha3: number | null
  beta1: number | null
  beta2: number | null
  beta3: number | null
  pwv1: number | null
  pwv2: number | null
  pwv3: number | null
}
export type AtmosphereResponse = {
  config: AtmosphereConfig
  station_labels: Record<string, string>
  adc_labels: Record<string, string>
  t_eff_points: AtmosphereTeffPoint[]
  measurement_points: AtmosphereMeasurementPoint[]
  raw_count: number
  bucket_seconds: number
  bucket_label: string
  aggregated: boolean
  temp_outlier_filter?: TempOutlierFilterStats
}
