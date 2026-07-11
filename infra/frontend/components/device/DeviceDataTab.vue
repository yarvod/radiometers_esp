<template>
    <div class="card" >
      <div class="card-head">
        <h3>История измерений</h3>
        <span class="badge">Графики</span>
      </div>
      <div class="inline fields">
        <label class="compact">С
          <input
            type="datetime-local"
            class="date-input"
            v-model="historyFilters.from"
            @focus="historyDateEditing.from = true"
            @blur="validateHistoryDate('from')"
          />
        </label>
        <label class="compact">По
          <input
            type="datetime-local"
            class="date-input"
            v-model="historyFilters.to"
            :disabled="historyAutoRefresh"
            @focus="historyDateEditing.to = true"
            @blur="validateHistoryDate('to')"
          />
        </label>
        <label class="compact">Лимит
          <input type="number" min="100" max="10000" step="100" v-model.number="historyFilters.limit" />
        </label>
      </div>
      <p class="muted" v-if="historyDateError">{{ historyDateError }}</p>
      <p class="muted" v-if="browserTzLabel">Таймзона браузера: {{ browserTzLabel }}</p>
      <div class="inline fields">
        <label class="compact">Усреднение
          <select v-model="historyFilters.bucketMode">
            <option value="auto">Авто</option>
            <option value="manual">Вручную</option>
          </select>
        </label>
        <label class="compact" v-if="historyFilters.bucketMode === 'manual'">Окно
          <div class="inline">
            <input type="number" min="1" step="1" v-model.number="historyFilters.bucketValue" />
            <select v-model="historyFilters.bucketUnit">
              <option value="s">сек</option>
              <option value="m">мин</option>
              <option value="h">ч</option>
            </select>
          </div>
        </label>
      </div>
      <div class="inline fields">
        <label class="checkbox">
          <input type="checkbox" v-model="tempOutlierFilter.enabled" />
          <span>Фильтр выбросов температуры</span>
        </label>
        <label class="compact" v-if="tempOutlierFilter.enabled">Окно, точек
          <input type="number" min="3" max="501" step="2" v-model.number="tempOutlierFilter.window" />
        </label>
        <label class="compact" v-if="tempOutlierFilter.enabled">Порог σ
          <input type="number" min="0.1" max="100" step="0.1" v-model.number="tempOutlierFilter.threshold" />
        </label>
        <label class="compact" v-if="tempOutlierFilter.enabled">Мин. соседей
          <input type="number" min="1" max="500" step="1" v-model.number="tempOutlierFilter.minCount" />
        </label>
      </div>
      <p class="muted" v-if="historyRangeLabel">{{ historyRangeLabel }}</p>
      <div class="actions">
        <button class="btn primary" @click="loadHistory" :disabled="historyLoading">Загрузить</button>
        <span class="muted" v-if="historyStatus">{{ historyStatus }}</span>
        <span class="muted" v-if="atmosphereStatus">{{ atmosphereStatus }}</span>
      </div>
      <div class="inline fields">
        <label class="checkbox">
          <input type="checkbox" v-model="historyAutoRefresh" />
          <span>Автообновление</span>
        </label>
        <label class="compact">Интервал (сек)
          <input type="number" min="5" max="300" step="5" v-model.number="historyRefreshSec" />
        </label>
      </div>
      <div class="status-row" v-if="historyBucketLabel && historyBucketLabel !== 'raw'">
        <span class="chip subtle">Окно: {{ historyBucketLabel }}</span>
        <span class="chip subtle">Исходно: {{ historyRawCount }}</span>
      </div>
      <div class="status-row" v-if="historyOutlierStatus">
        <span class="chip subtle">{{ historyOutlierStatus }}</span>
      </div>
      <div class="form-group">
        <label>Температурные датчики</label>
        <div class="chip-select">
          <button
            v-for="sensor in historyTempOptions"
            :key="`history-temp-${sensor.idx}`"
            type="button"
            class="chip-option"
            :class="{ selected: historySelection.tempIndices.includes(sensor.idx) }"
            @click="toggleHistoryTemp(sensor.idx)"
          >
            {{ sensor.label }}
          </button>
          <span v-if="historyTempOptions.length === 0" class="muted">Нет датчиков</span>
        </div>
      </div>
      <div class="form-group">
        <label>Серии ADC</label>
        <div class="chip-select">
          <button
            v-for="series in adcSeriesOptions"
            :key="series.key"
            type="button"
            class="chip-option"
            :class="{ selected: historySelection.adcSeries.includes(series.key) }"
            @click="toggleAdcSeries(series.key)"
          >
            {{ series.label }}
          </button>
        </div>
      </div>
      <div class="form-group atmosphere-coefficients">
        <label>Коэффициенты PWV</label>
        <div class="inline fields">
          <label class="compact">Сезон
            <select v-model="configForm.atmosphere.season" @change="markAtmosphereConfigDirty">
              <option value="auto">Авто по дате</option>
              <option value="summer">Лето</option>
              <option value="winter">Зима</option>
            </select>
          </label>
          <div class="actions coefficient-actions">
            <button class="btn primary sm" type="button" @click="loadAtmosphere">Пересчитать PWV</button>
            <button class="btn ghost sm" type="button" @click="saveConfig" :disabled="configSaving">Сохранить коэффициенты</button>
          </div>
        </div>
        <div class="coefficient-grid">
          <div class="coefficient-row" v-for="key in atmosphereBandKeys" :key="`coeff-${key}`">
            <div class="coefficient-channel">{{ atmosphereBandLabel(key) }}</div>
            <label class="compact">Канал
              <select v-model="configForm.atmosphere.bands[key].mode" @change="markAtmosphereConfigDirty">
                <option value="off">Не считать</option>
                <option value="auto">Авто</option>
                <option value="2mm">2 mm</option>
                <option value="3mm">3 mm</option>
                <option value="manual">Alpha/Beta вручную</option>
              </select>
            </label>
            <label class="compact">alpha
              <input
                type="number"
                step="0.001"
                v-model.number="configForm.atmosphere.bands[key].alpha"
                :placeholder="atmosphereCoefficientPlaceholder(key, 'alpha')"
                @input="markAtmosphereConfigDirty"
              />
            </label>
            <label class="compact">beta
              <input
                type="number"
                step="0.001"
                v-model.number="configForm.atmosphere.bands[key].beta"
                :placeholder="atmosphereCoefficientPlaceholder(key, 'beta')"
                @input="markAtmosphereConfigDirty"
              />
            </label>
            <button class="btn ghost sm coefficient-reset" type="button" @click="clearAtmosphereBandOverrides(key)">
              Табличные
            </button>
            <div class="coefficient-used">{{ atmosphereDefaultUsage(key) }}</div>
            <div class="coefficient-used">{{ atmosphereCoefficientUsage(key) }}</div>
          </div>
        </div>
      </div>
      <MeasurementHistoryPanel :definitions="measurementChartDefinitions" />
      <AtmosphereHistoryPanel
        :teff="atmosphereChartDefinitions.teff"
        :tau="atmosphereChartDefinitions.tau"
        :pwv="atmosphereChartDefinitions.pwv"
        v-model:average="atmosphereAverage"
        v-model:primary-station="atmospherePrimaryStation"
        :stations="atmosphereStationOptions"
        :gnss-datasets="gnssDataSets"
        :selected-gnss-ids="gnssSelectedIds"
        :gnss-status="gnssStatus"
        :pwv-status="pwvRadiometerStatus"
        @toggle-gnss="toggleGnssDataset"
      />
    </div>

</template>

<script setup lang="ts">
import AtmosphereHistoryPanel from './data/AtmosphereHistoryPanel.vue'
import MeasurementHistoryPanel from './data/MeasurementHistoryPanel.vue'
import type {
  AtmosphereBandConfig,
  AtmosphereBandKey,
  DeviceConfig,
  GnssData,
  GnssDataSeries,
  GnssDataSeriesResponse,
} from '~/types/device'
import type {
  AtmosphereCoefficientDefaults,
  AtmosphereCoefficientPair,
  AtmosphereMeasurementPoint,
  AtmosphereResponse,
  MeasurementPoint,
  MeasurementsResponse,
  StationOption,
  TempOutlierFilterStats,
} from '~/types/device-data'
import type { HistoryChartDefinition } from '~/types/charts'
import { browserTimezoneLabel, localInputToIso, toLocalInputValue } from '~/utils/datetime'
const { apiFetch } = useApi()
const props = defineProps<{
  deviceId: string
  config: DeviceConfig
  liveTemps: Array<{ key: string; label: string; value: unknown; address: string }>
  gnssRevision?: number
}>()
const emit = defineEmits<{
  'config-updated': [config: DeviceConfig]
}>()
const deviceId = computed(() => props.deviceId)
const deviceConfig = computed(() => props.config)
const configDirty = ref(false)
const configStatus = ref('')
const configSaving = ref(false)
const configForm = reactive({
  atmosphere: {
    stationIds: [] as string[],
    altitudeM: 0,
    h0M: 5300,
    season: 'auto',
    bands: {
      adc2: { mode: '2mm', alpha: null as number | null, beta: null as number | null },
      adc3: { mode: '3mm', alpha: null as number | null, beta: null as number | null },
    } as Record<AtmosphereBandKey, { mode: string; alpha: number | null; beta: number | null }>,
  },
})

const buildTempLabelMap = (
  config?: Pick<DeviceConfig, 'temp_labels' | 'temp_addresses' | 'temp_label_map'> | null,
) => {
  const labelByAddress = new Map<string, string>()
  Object.entries(config?.temp_label_map || {}).forEach(([address, label]) => {
    if (address && label) labelByAddress.set(address, label)
  })
  const labels = config?.temp_labels || []
  const addresses = config?.temp_addresses || []
  addresses.forEach((address, idx) => {
    if (address && !labelByAddress.has(address)) {
      labelByAddress.set(address, labels[idx] || `t${idx + 1}`)
    }
  })
  return labelByAddress
}
const tempEntries = computed(() => props.liveTemps)
const historyTempOptions = computed(() => {
  const labels = historyTempLabels.value.length
    ? historyTempLabels.value
    : deviceConfig.value?.temp_labels?.length
    ? deviceConfig.value.temp_labels
    : tempEntries.value.map((entry, idx) => entry.label || `t${idx + 1}`)
  return labels.map((label, idx) => ({ label, idx }))
})
const adcLabelMap = computed(() => deviceConfig.value?.adc_labels || {})
const adcLabelDefaults: Record<string, string> = {
  adc1: 'ADC1',
  adc2: 'ADC2',
  adc3: 'ADC3',
  adc1_cal: 'ADC1 Cal',
  adc2_cal: 'ADC2 Cal',
  adc3_cal: 'ADC3 Cal',
}
const adcLabel = (key: string, fallback: string) => adcLabelMap.value[key] || fallback
const atmosphereBandKeys: AtmosphereBandKey[] = ['adc2', 'adc3']
const atmosphereBandLabel = (key: AtmosphereBandKey) => `${adcLabel(key, key.toUpperCase())} (${key.toUpperCase()})`
const tempBindingRoles = [
  { key: 'radiometer_adc1' },
  { key: 'radiometer_adc2' },
  { key: 'radiometer_adc3' },
  { key: 'calibration_load' },
]
const atmosphereStationOptions = computed(() => {
  const ids = deviceConfig.value?.atmosphere_config?.station_ids || []
  const byId = new Map(stationOptions.value.map((station) => [station.station_id, station]))
  return ids.map((id) => byId.get(id) || { station_id: id, name: atmosphereData.value?.station_labels?.[id] || null })
})
const selectedAtmosphereStations = computed(() => {
  const byId = new Map(stationOptions.value.map((station) => [station.station_id, station]))
  return configForm.atmosphere.stationIds.map((id) => (
    byId.get(id) || { station_id: id, name: atmosphereData.value?.station_labels?.[id] || null }
  ))
})
const historyFilters = reactive({
  from: '',
  to: '',
  limit: 2000,
  bucketMode: 'auto',
  bucketValue: 10,
  bucketUnit: 's',
})
const tempOutlierFilter = reactive({
  enabled: false,
  window: 9,
  threshold: 3.5,
  minCount: 5,
})
const historySelection = reactive({
  tempIndices: [] as number[],
  adcSeries: ['adc1', 'adc2', 'adc3'] as string[],
})
const historyData = ref<MeasurementPoint[]>([])
const historyTempLabels = ref<string[]>([])
const historyTempAddresses = ref<string[]>([])
const historyTempBindings = ref<Record<string, string>>({})
const historyBrightnessLabels = ref<Record<string, string>>({})
const atmosphereData = ref<AtmosphereResponse | null>(null)
const gnssDataSets = ref<GnssData[]>([])
const gnssSeries = ref<GnssDataSeries[]>([])
const gnssSelectedIds = ref<string[]>([])
const gnssLoading = ref(false)
const gnssStatus = ref('')
const atmosphereCoefficientDefaults = ref<AtmosphereCoefficientDefaults | null>(null)
const atmosphereStatus = ref('')
const atmosphereAverage = ref(false)
const atmospherePrimaryStation = ref('')
const stationOptions = ref<StationOption[]>([])
const stationOptionsLoading = ref(false)
const stationSearchQuery = ref('')
const stationSelectOpen = ref(false)
let stationSearchTimer: ReturnType<typeof setTimeout> | null = null
let atmosphereReloadTimer: ReturnType<typeof setTimeout> | null = null
const historyLoading = ref(false)
const historyStatus = ref('')
const historyBucketLabel = ref('')
const historyRawCount = ref(0)
const historyOutlierFilter = ref<TempOutlierFilterStats | null>(null)
const historyAutoRefresh = ref(false)
const historyRefreshSec = ref(15)
let historyTimer: ReturnType<typeof setInterval> | null = null

const maskToIndices = (mask: number, count: number) => {
  const out: number[] = []
  for (let i = 0; i < count; i++) {
    if (mask & (1 << i)) out.push(i)
  }
  return out
}

const normalizeIndices = (indices: number[], count: number) => {
  const set = new Set(indices.filter((idx) => idx >= 0 && idx < count))
  return Array.from(set).sort((a, b) => a - b)
}

const parseLocalInput = (value: string) => {
  if (!value) return null
  const match = value.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})$/)
  if (!match) return null
  const year = Number(match[1])
  const month = Number(match[2])
  const day = Number(match[3])
  const hour = Number(match[4])
  const minute = Number(match[5])
  if (![year, month, day, hour, minute].every((n) => Number.isFinite(n))) return null
  const dt = new Date(year, month - 1, day, hour, minute, 0, 0)
  if (Number.isNaN(dt.getTime())) return null
  if (dt.getFullYear() !== year || dt.getMonth() !== month - 1 || dt.getDate() !== day) return null
  return dt
}

const parseDateAny = (value: string) => {
  if (!value) return null
  const local = parseLocalInput(value)
  if (local) return local
  const dt = new Date(value)
  if (Number.isNaN(dt.getTime())) return null
  return dt
}

const formatTimestamp = (value: string) => {
  const dt = parseDateAny(value)
  if (!dt || Number.isNaN(dt.getTime())) return value
  return dt.toLocaleString('ru-RU', {
    day: '2-digit',
    month: '2-digit',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  })
}
const formatOptionalDate = (value: string | null) => value ? formatTimestamp(value) : '—'

const stationLabel = (station: StationOption) => {
  return station.name ? `${station.station_id} · ${station.name}` : station.station_id
}

const finiteOrNull = (value: unknown) => {
  if (value === null || value === undefined || value === '') return null
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : null
}

const normalizedTempOutlierFilterParams = () => {
  let window = Math.max(3, Math.min(501, Math.floor(Number(tempOutlierFilter.window) || 9)))
  if (window % 2 === 0) window += 1
  const threshold = Math.max(0.1, Math.min(100, Number(tempOutlierFilter.threshold) || 3.5))
  const minCount = Math.max(1, Math.min(window - 1, Math.floor(Number(tempOutlierFilter.minCount) || 5)))
  tempOutlierFilter.window = window
  tempOutlierFilter.threshold = threshold
  tempOutlierFilter.minCount = minCount
  return { window, threshold, minCount }
}

const appendTempOutlierFilterParams = (params: URLSearchParams) => {
  if (!tempOutlierFilter.enabled) return
  const normalized = normalizedTempOutlierFilterParams()
  params.set('temp_outlier_filter', 'true')
  params.set('temp_outlier_window', String(normalized.window))
  params.set('temp_outlier_threshold', String(normalized.threshold))
  params.set('temp_outlier_min_count', String(normalized.minCount))
}

const historyOutlierStatus = computed(() => {
  const stats = historyOutlierFilter.value
  if (!stats?.enabled) return ''
  const sensors = (stats.inspected_indices || []).map((idx) => historyTempLabels.value[idx] || `t${idx + 1}`).join(', ')
  return `Фильтр температуры: убрано ${stats.removed_count} из ${stats.input_count}; датчики ${sensors || '—'}`
})

const normalizedSeason = (value: unknown) => {
  const season = String(value || 'auto')
  return season === 'summer' || season === 'winter' ? season : 'auto'
}

const normalizedBandMode = (value: unknown, fallback: string) => {
  const mode = String(value || fallback)
  return ['off', 'auto', '2mm', '3mm', 'manual'].includes(mode) ? mode : fallback
}

const defaultBandForAtmosphereKey = (key: AtmosphereBandKey) => (key === 'adc2' ? '2mm' : '3mm')

const selectedBandForAtmosphereKey = (key: AtmosphereBandKey) => {
  const mode = normalizedBandMode(configForm.atmosphere.bands[key].mode, defaultBandForAtmosphereKey(key))
  return mode === '3mm' || mode === '2mm' ? mode : defaultBandForAtmosphereKey(key)
}

const selectedSeasonForAtmosphereDefaults = () => {
  const season = normalizedSeason(configForm.atmosphere.season)
  if (season === 'summer' || season === 'winter') return season
  const timestamp = atmosphereData.value?.measurement_points?.at(-1)?.timestamp || historyData.value.at(-1)?.timestamp
  const dt = timestamp ? parseDateAny(timestamp) : new Date()
  const month = dt ? dt.getMonth() + 1 : new Date().getMonth() + 1
  return month >= 5 && month <= 10 ? 'summer' : 'winter'
}

const interpolateCoefficient = (band: string, season: 'summer' | 'winter'): AtmosphereCoefficientPair | null => {
  const rows = atmosphereCoefficientDefaults.value?.table?.[band] || []
  if (!rows.length) return atmosphereCoefficientDefaults.value?.channels?.[band]?.seasons?.[season] || null
  const heightKm = Math.max(0, Number(configForm.atmosphere.altitudeM) || 0) / 1000
  if (heightKm <= rows[0].height_km) return rows[0][season]
  const last = rows[rows.length - 1]
  if (heightKm >= last.height_km) return last[season]
  for (let i = 0; i < rows.length - 1; i += 1) {
    const a = rows[i]
    const b = rows[i + 1]
    if (heightKm >= a.height_km && heightKm <= b.height_km) {
      const k = (heightKm - a.height_km) / (b.height_km - a.height_km)
      return {
        alpha: Number(a[season].alpha) + (Number(b[season].alpha) - Number(a[season].alpha)) * k,
        beta: Number(a[season].beta) + (Number(b[season].beta) - Number(a[season].beta)) * k,
      }
    }
  }
  return null
}

const atmosphereDefaultPair = (key: AtmosphereBandKey, season = selectedSeasonForAtmosphereDefaults()) => (
  interpolateCoefficient(selectedBandForAtmosphereKey(key), season)
)

const atmosphereCoefficientPlaceholder = (key: AtmosphereBandKey, field: 'alpha' | 'beta') => {
  const pair = atmosphereDefaultPair(key)
  const value = pair?.[field]
  return value !== null && value !== undefined && Number.isFinite(value) ? String(value) : 'таблично'
}

const buildAtmosphereCoefficientPayload = () => {
  const bands: Record<string, { mode: string; alpha?: number; beta?: number }> = {}
  atmosphereBandKeys.forEach((key) => {
    const form = configForm.atmosphere.bands[key]
    const defaultMode = key === 'adc2' ? '2mm' : '3mm'
    const payload: { mode: string; alpha?: number; beta?: number } = {
      mode: normalizedBandMode(form.mode, defaultMode),
    }
    const alpha = finiteOrNull(form.alpha)
    const beta = finiteOrNull(form.beta)
    if (alpha !== null) payload.alpha = alpha
    if (beta !== null) payload.beta = beta
    bands[key] = payload
  })
  return {
    season: normalizedSeason(configForm.atmosphere.season),
    bands,
  }
}

const formatCoefficientValue = (value: number | null) => (
  value !== null && Number.isFinite(value) ? value.toFixed(4) : '—'
)

const atmosphereDefaultUsage = (key: AtmosphereBandKey) => {
  const summer = atmosphereDefaultPair(key, 'summer')
  const winter = atmosphereDefaultPair(key, 'winter')
  const selected = selectedSeasonForAtmosphereDefaults() === 'summer' ? 'лето' : 'зима'
  return [
    `Таблично (${selected}, ${selectedBandForAtmosphereKey(key)}): alpha ${formatCoefficientValue(atmosphereDefaultPair(key)?.alpha ?? null)}, beta ${formatCoefficientValue(atmosphereDefaultPair(key)?.beta ?? null)}`,
    `лето ${formatCoefficientValue(summer?.alpha ?? null)}/${formatCoefficientValue(summer?.beta ?? null)}`,
    `зима ${formatCoefficientValue(winter?.alpha ?? null)}/${formatCoefficientValue(winter?.beta ?? null)}`,
  ].join('; ')
}

const coefficientStats = (
  alphaKey: keyof AtmosphereMeasurementPoint,
  betaKey: keyof AtmosphereMeasurementPoint,
) => {
  const points = atmosphereData.value?.measurement_points || []
  const pairs = points
    .map((point) => {
      const alpha = Number(point[alphaKey])
      const beta = Number(point[betaKey])
      if (!Number.isFinite(alpha) || !Number.isFinite(beta)) return null
      return { alpha, beta }
    })
    .filter((item): item is { alpha: number; beta: number } => item !== null)
  if (!pairs.length) return null
  const latest = pairs[pairs.length - 1]
  const minAlpha = Math.min(...pairs.map((pair) => pair.alpha))
  const maxAlpha = Math.max(...pairs.map((pair) => pair.alpha))
  const minBeta = Math.min(...pairs.map((pair) => pair.beta))
  const maxBeta = Math.max(...pairs.map((pair) => pair.beta))
  const varying = Math.abs(maxAlpha - minAlpha) > 1e-9 || Math.abs(maxBeta - minBeta) > 1e-9
  return { latest, minAlpha, maxAlpha, minBeta, maxBeta, varying }
}

const atmosphereCoefficientUsage = (key: AtmosphereBandKey) => {
  const idx = key === 'adc2' ? '2' : '3'
  const stats = coefficientStats(
    `alpha${idx}` as keyof AtmosphereMeasurementPoint,
    `beta${idx}` as keyof AtmosphereMeasurementPoint,
  )
  if (!stats) return 'Используется: alpha —, beta —'
  if (!stats.varying) {
    return `Используется: alpha ${formatCoefficientValue(stats.latest.alpha)}, beta ${formatCoefficientValue(stats.latest.beta)}`
  }
  return [
    `Используется: alpha ${formatCoefficientValue(stats.latest.alpha)}, beta ${formatCoefficientValue(stats.latest.beta)}`,
    `диапазон alpha ${formatCoefficientValue(stats.minAlpha)}-${formatCoefficientValue(stats.maxAlpha)}`,
    `beta ${formatCoefficientValue(stats.minBeta)}-${formatCoefficientValue(stats.maxBeta)}`,
  ].join('; ')
}

const clearAtmosphereBandOverrides = (key: AtmosphereBandKey) => {
  configForm.atmosphere.bands[key].alpha = null
  configForm.atmosphere.bands[key].beta = null
  markAtmosphereConfigDirty()
}

const scheduleAtmosphereReload = () => {
  if (atmosphereReloadTimer) clearTimeout(atmosphereReloadTimer)
  atmosphereReloadTimer = setTimeout(() => {
    atmosphereReloadTimer = null
    loadAtmosphere()
  }, 350)
}

const markAtmosphereConfigDirty = () => {
  configDirty.value = true
  scheduleAtmosphereReload()
}

const formatRangeDate = (value: string) => {
  const dt = parseDateAny(value)
  if (!dt || Number.isNaN(dt.getTime())) return value
  return dt.toLocaleString('ru-RU', {
    day: '2-digit',
    month: '2-digit',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  })
}

const historyRangeLabel = computed(() => {
  if (!historyFilters.from) return ''
  const fromLabel = formatRangeDate(historyFilters.from)
  if (historyAutoRefresh.value) return `Диапазон: ${fromLabel} — сейчас`
  if (!historyFilters.to) return ''
  return `Диапазон: ${fromLabel} — ${formatRangeDate(historyFilters.to)}`
})

const historyDateEditing = reactive({ from: false, to: false })
const historyDateError = ref('')

const validateHistoryDate = (field: 'from' | 'to') => {
  historyDateEditing[field] = false
  const value = field === 'from' ? historyFilters.from : historyFilters.to
  if (!value) {
    historyDateError.value = ''
    return true
  }
  const dt = parseDateAny(value)
  if (!dt || Number.isNaN(dt.getTime())) {
    historyDateError.value = 'Введите корректную дату и время'
    return false
  }
  historyDateError.value = ''
  return true
}

const setHistoryWindow = (end: Date) => {
  historyDateEditing.from = false
  historyDateEditing.to = false
  historyFilters.to = toLocalInputValue(end)
  historyFilters.from = toLocalInputValue(new Date(end.getTime() - 24 * 60 * 60 * 1000))
}

const palette = [
  '#1f77b4',
  '#ff7f0e',
  '#2ca02c',
  '#d62728',
  '#9467bd',
  '#8c564b',
  '#e377c2',
  '#7f7f7f',
  '#bcbd22',
  '#17becf',
]
const atmosphereRadiometerPalette = [palette[0], palette[1], palette[2]]
const pwvProfilePalette = palette.slice(3)
const gnssPalette = ['#111827', '#0f766e', '#b45309', '#be123c', '#4338ca', '#047857']

type AdcSeriesOption = {
  key: string
  label: string
  color: string
  extract: (row: MeasurementPoint) => number | null
}

const adcSeriesOptions = computed<AdcSeriesOption[]>(() => [
  { key: 'adc1', label: adcLabelMap.value.adc1 || 'ADC1', color: '#1f77b4', extract: (row) => row.adc1 ?? null },
  { key: 'adc2', label: adcLabelMap.value.adc2 || 'ADC2', color: '#2ca02c', extract: (row) => row.adc2 ?? null },
  { key: 'adc3', label: adcLabelMap.value.adc3 || 'ADC3', color: '#d62728', extract: (row) => row.adc3 ?? null },
  { key: 'adc1_cal', label: adcLabelMap.value.adc1_cal || 'ADC1 Cal', color: '#9ecae1', extract: (row) => row.adc1_cal ?? null },
  { key: 'adc2_cal', label: adcLabelMap.value.adc2_cal || 'ADC2 Cal', color: '#98df8a', extract: (row) => row.adc2_cal ?? null },
  { key: 'adc3_cal', label: adcLabelMap.value.adc3_cal || 'ADC3 Cal', color: '#ff9896', extract: (row) => row.adc3_cal ?? null },
])

const normalizeHistorySelection = (indices: number[], count: number) => {
  const normalized = normalizeIndices(indices, count)
  return normalized.length ? normalized : (count ? [0] : [])
}

const historyLabels = computed(() => historyData.value.map((row) => formatTimestamp(row.timestamp)))

const buildDataset = (label: string, data: (number | null)[], color: string) => ({
  label,
  data,
  borderColor: color,
  backgroundColor: color,
  borderWidth: 2,
  tension: 0.25,
  pointRadius: 0,
})

const buildTempDatasets = () => {
  const labels = historyTempOptions.value.map((entry) => entry.label)
  const selected = normalizeHistorySelection(historySelection.tempIndices, labels.length)
  return selected.map((idx, seriesIdx) => {
    const color = palette[seriesIdx % palette.length]
    const data = historyData.value.map((row) => {
      const value = row.temps?.[idx]
      return Number.isFinite(value) ? Number(value) : null
    })
    const label = labels[idx] || `t${idx + 1}`
    return buildDataset(label, data, color)
  })
}

const buildAdcDatasets = () => {
  return adcSeriesOptions.value
    .filter((series) => historySelection.adcSeries.includes(series.key))
    .map((series) => buildDataset(series.label, historyData.value.map((row) => series.extract(row)), series.color))
}

const buildBrightnessDatasets = () => {
  const options: AdcSeriesOption[] = [
    {
      key: 'brightness_temp1',
      label: historyBrightnessLabels.value.brightness_temp1 || `${adcLabelMap.value.adc1 || 'ADC1'} Tk`,
      color: '#16a085',
      extract: (row) => row.brightness_temp1 ?? null,
    },
    {
      key: 'brightness_temp2',
      label: historyBrightnessLabels.value.brightness_temp2 || `${adcLabelMap.value.adc2 || 'ADC2'} Tk`,
      color: '#c0392b',
      extract: (row) => row.brightness_temp2 ?? null,
    },
    {
      key: 'brightness_temp3',
      label: historyBrightnessLabels.value.brightness_temp3 || `${adcLabelMap.value.adc3 || 'ADC3'} Tk`,
      color: '#8e44ad',
      extract: (row) => row.brightness_temp3 ?? null,
    },
  ]
  return options
    .map((series) => buildDataset(series.label, historyData.value.map((row) => series.extract(row)), series.color))
    .filter((dataset) => dataset.data.some((value) => Number.isFinite(value)))
}

const loadTempAddress = computed(() => historyTempBindings.value.calibration_load || deviceConfig.value?.temp_bindings?.calibration_load || '')
const loadTempIndex = computed(() => {
  const address = loadTempAddress.value
  if (!address) return -1
  const addresses = historyTempAddresses.value.length ? historyTempAddresses.value : (deviceConfig.value?.temp_addresses || [])
  return addresses.findIndex((item) => item === address)
})
const loadTempLabel = computed(() => {
  const idx = loadTempIndex.value
  if (idx < 0) return 'T нагрузки, K'
  const label = historyTempLabels.value[idx] || deviceConfig.value?.temp_labels?.[idx] || `t${idx + 1}`
  return `${label}, K`
})

const buildLoadCheckDatasets = () => {
  const calOptions: AdcSeriesOption[] = [
    {
      key: 'cal_brightness_temp1',
      label: `${adcLabelMap.value.adc1 || 'ADC1'} Tk по Cal`,
      color: '#16a085',
      extract: (row) => row.cal_brightness_temp1 ?? null,
    },
    {
      key: 'cal_brightness_temp2',
      label: `${adcLabelMap.value.adc2 || 'ADC2'} Tk по Cal`,
      color: '#c0392b',
      extract: (row) => row.cal_brightness_temp2 ?? null,
    },
    {
      key: 'cal_brightness_temp3',
      label: `${adcLabelMap.value.adc3 || 'ADC3'} Tk по Cal`,
      color: '#8e44ad',
      extract: (row) => row.cal_brightness_temp3 ?? null,
    },
  ]
  const datasets = calOptions
    .map((series) => buildDataset(series.label, historyData.value.map((row) => series.extract(row)), series.color))
    .filter((dataset) => dataset.data.some((value) => Number.isFinite(value)))
  const idx = loadTempIndex.value
  if (idx >= 0) {
    const data = historyData.value.map((row) => {
      const value = row.temps?.[idx]
      return Number.isFinite(value) ? Number(value) + 273.15 : null
    })
    const loadDataset = buildDataset(loadTempLabel.value, data, '#111827')
    loadDataset.borderWidth = 3
    loadDataset.borderDash = [6, 4]
    datasets.unshift(loadDataset)
  }
  return datasets
}

type ChartTimelineEntry = {
  key: string
  label: string
  sort: number
}

const timelineSortValue = (value: string) => {
  const dt = parseDateAny(value)
  return dt && !Number.isNaN(dt.getTime()) ? dt.getTime() : Number.MAX_SAFE_INTEGER
}

const buildTimeline = (entries: { key: string; timestamp: string }[]) => {
  const byKey = new Map<string, ChartTimelineEntry>()
  entries.forEach((entry) => {
    if (!byKey.has(entry.key)) {
      byKey.set(entry.key, {
        key: entry.key,
        label: formatTimestamp(entry.timestamp),
        sort: timelineSortValue(entry.timestamp),
      })
    }
  })
  return Array.from(byKey.values()).sort((a, b) => a.sort - b.sort)
}

const dateTimeKey = (value: string) => {
  const dt = parseDateAny(value)
  return dt && !Number.isNaN(dt.getTime()) ? dt.toISOString() : value
}

const buildAtmosphereDataset = (label: string, data: (number | null)[], color: string) => {
  const dataset = buildDataset(label, data, color)
  dataset.tension = 0.2
  dataset.pointRadius = 1.5
  dataset.spanGaps = true
  return dataset
}

const mapTimelineValues = (timeline: ChartTimelineEntry[], values: Map<string, number>) => (
  timeline.map((entry) => values.get(entry.key) ?? null)
)

const buildTeffDatasets = () => {
  const response = atmosphereData.value
  if (!response) return { labels: [], datasets: [] }
  const timeline = buildTimeline(response.t_eff_points.map((point) => ({
    key: dateTimeKey(point.sounding_time),
    timestamp: point.sounding_time,
  })))
  const byStation = new Map<string, Map<string, number>>()
  response.t_eff_points.forEach((point) => {
    if (!Number.isFinite(point.t_eff)) return
    if (!byStation.has(point.station_id)) byStation.set(point.station_id, new Map())
    byStation.get(point.station_id)!.set(dateTimeKey(point.sounding_time), Number(point.t_eff))
  })
  return {
    labels: timeline.map((entry) => entry.label),
    datasets: Array.from(byStation.entries()).map(([stationId, values], idx) =>
      buildAtmosphereDataset(response.station_labels[stationId] || stationId, mapTimelineValues(timeline, values), palette[idx % palette.length])
    ),
  }
}

const atmosphereAdcSeries = computed(() => {
  const items = [
    { key: '1', adcKey: 'adc1', label: adcLabelMap.value.adc1 || 'ADC1', color: atmosphereRadiometerPalette[0] },
    { key: '2', adcKey: 'adc2', label: adcLabelMap.value.adc2 || 'ADC2', color: atmosphereRadiometerPalette[1] },
    { key: '3', adcKey: 'adc3', label: adcLabelMap.value.adc3 || 'ADC3', color: atmosphereRadiometerPalette[2] },
  ]
  const labelCounts = new Map<string, number>()
  items.forEach((item) => labelCounts.set(item.label, (labelCounts.get(item.label) || 0) + 1))
  return items.map((item) => ({
    ...item,
    label: (labelCounts.get(item.label) || 0) > 1 ? `${item.label} ${item.adcKey.toUpperCase()}` : item.label,
  }))
})

const buildTauDatasets = () => {
  const points = atmosphereData.value?.measurement_points || []
  const timeline = buildTimeline(points.map((point, idx) => ({ key: `m:${idx}`, timestamp: point.timestamp })))
  return {
    labels: timeline.map((entry) => entry.label),
    datasets: atmosphereAdcSeries.value
      .map((series) => {
        const key = `tau${series.key}` as keyof AtmosphereMeasurementPoint
        const data = points.map((point) => {
          const y = point[key]
          return Number.isFinite(y) ? Number(y) : null
        })
        return buildAtmosphereDataset(`${series.label} tau`, data, series.color)
      })
      .filter((dataset) => dataset.data.some((value: number | null) => Number.isFinite(value))),
  }
}

const buildPwvAtmosphereDatasets = () => {
  const response = atmosphereData.value
  const measurementEntries = (response?.measurement_points || []).map((point, idx) => ({
    key: `m:${idx}`,
    timestamp: point.timestamp,
  }))
  const profileEntries = (response?.t_eff_points || []).map((point) => ({
    key: `p:${point.station_id}:${dateTimeKey(point.sounding_time)}`,
    timestamp: point.sounding_time,
  }))
  const gnssEntries = gnssSeries.value.flatMap((series) =>
    series.points.map((point) => ({
      key: `g:${series.dataset.id}:${dateTimeKey(point.measured_at)}`,
      timestamp: point.measured_at,
    })),
  )
  const timeline = buildTimeline([...measurementEntries, ...profileEntries, ...gnssEntries])
  if (!timeline.length) return { labels: [], datasets: [] }
  const radiometerDatasets = response
    ? atmosphereAdcSeries.value
        .map((series) => {
          const key = `pwv${series.key}` as keyof AtmosphereMeasurementPoint
          const values = new Map<string, number>()
          response.measurement_points.forEach((point, idx) => {
            const y = point[key]
            if (Number.isFinite(y)) values.set(`m:${idx}`, Number(y))
          })
          return buildAtmosphereDataset(`${series.label} PWV`, mapTimelineValues(timeline, values), series.color)
        })
        .filter((dataset) => dataset.data.some((value: number | null) => Number.isFinite(value)))
    : []
  const byStation = new Map<string, Map<string, number>>()
  ;(response?.t_eff_points || []).forEach((point) => {
    if (!Number.isFinite(point.pwv_profile)) return
    if (!byStation.has(point.station_id)) byStation.set(point.station_id, new Map())
    byStation.get(point.station_id)!.set(`p:${point.station_id}:${dateTimeKey(point.sounding_time)}`, Number(point.pwv_profile))
  })
  const profileDatasets = Array.from(byStation.entries()).map(([stationId, values], idx) => {
    const dataset = buildAtmosphereDataset(
      `Профиль ${response.station_labels[stationId] || stationId}`,
      mapTimelineValues(timeline, values),
      pwvProfilePalette[idx % pwvProfilePalette.length],
    )
    dataset.borderDash = [6, 4]
    dataset.pointRadius = 2
    return dataset
  })
  const gnssDatasets = gnssSeries.value
    .map((series, idx) => {
      const values = new Map<string, number>()
      series.points.forEach((point) => {
        if (Number.isFinite(point.pw_mm)) {
          values.set(`g:${series.dataset.id}:${dateTimeKey(point.measured_at)}`, Number(point.pw_mm))
        }
      })
      const dataset = buildAtmosphereDataset(
        `${series.dataset.name} GNSS`,
        mapTimelineValues(timeline, values),
        gnssPalette[idx % gnssPalette.length],
      )
      dataset.pointRadius = 2
      dataset.borderWidth = 2
      return dataset
    })
    .filter((dataset) => dataset.data.some((value: number | null) => Number.isFinite(value)))
  return {
    labels: timeline.map((entry) => entry.label),
    datasets: [...radiometerDatasets, ...profileDatasets, ...gnssDatasets],
  }
}

const countFiniteAtmosphereValues = (key: keyof AtmosphereMeasurementPoint) => {
  const points = atmosphereData.value?.measurement_points || []
  return points.reduce((count, point) => count + (Number.isFinite(point[key]) ? 1 : 0), 0)
}

const pwvRadiometerStatus = computed(() => {
  if (!atmosphereData.value?.measurement_points?.length) return ''
  const tau2 = countFiniteAtmosphereValues('tau2')
  const tau3 = countFiniteAtmosphereValues('tau3')
  const pwv2 = countFiniteAtmosphereValues('pwv2')
  const pwv3 = countFiniteAtmosphereValues('pwv3')
  if (pwv2 || pwv3) return `Радиометр: ADC2 ${pwv2}, ADC3 ${pwv3}`
  if (tau2 || tau3) return 'PWV радиометров нет: проверь режим и alpha/beta для ADC2/ADC3'
  return 'PWV радиометров нет: нет tau по ADC2/ADC3'
})

const measurementChartDefinitions = computed<HistoryChartDefinition[]>(() => [
  { key: 'temp', title: 'Температуры', labels: historyLabels.value, datasets: buildTempDatasets() },
  { key: 'adc', title: 'ADC + Cal', labels: historyLabels.value, datasets: buildAdcDatasets() },
  { key: 'brightness', title: 'Яркостная температура Tk', labels: historyLabels.value, datasets: buildBrightnessDatasets() },
  { key: 'loadCheck', title: 'Контроль теплой нагрузки', labels: historyLabels.value, datasets: buildLoadCheckDatasets() },
])

const atmosphereChartDefinitions = computed<Record<'teff' | 'tau' | 'pwv', HistoryChartDefinition>>(() => {
  const teff = buildTeffDatasets()
  const tau = buildTauDatasets()
  const pwv = buildPwvAtmosphereDatasets()
  return {
    teff: { key: 'teff', title: 'Эффективная температура по зондам', ...teff, atmosphere: true },
    tau: { key: 'tau', title: 'Тау атмосферы', ...tau, atmosphere: true },
    pwv: { key: 'pwv', title: 'PWV', ...pwv, atmosphere: true },
  }
})

watch(
  () => historyTempOptions.value.length,
  (count) => {
    historySelection.tempIndices = normalizeHistorySelection(historySelection.tempIndices, count)
  }
)

watch(
  () => [historyAutoRefresh.value, historyRefreshSec.value],
  () => {
    if (historyAutoRefresh.value) {
      startHistoryTimer()
      loadHistory()
    } else {
      stopHistoryTimer()
    }
  }
)

watch(
  () => [atmospherePrimaryStation.value, atmosphereAverage.value],
  () => {
    if (!deviceConfig.value?.atmosphere_config?.station_ids?.length) return
    if (!historyFilters.from) return
    loadAtmosphere()
  }
)

watch(
  () => stationSearchQuery.value,
  () => {
    if (stationSearchTimer) clearTimeout(stationSearchTimer)
    stationSearchTimer = setTimeout(() => {
      loadStationOptions()
    }, 300)
  }
)

function toggleHistoryTemp(idx: number) {
  const count = historyTempOptions.value.length
  if (historySelection.tempIndices.includes(idx)) {
    if (historySelection.tempIndices.length <= 1) {
      return
    }
    historySelection.tempIndices = historySelection.tempIndices.filter((value) => value !== idx)
  } else {
    historySelection.tempIndices = normalizeIndices([...historySelection.tempIndices, idx], count)
  }
}

function toggleAdcSeries(key: string) {
  const selected = new Set(historySelection.adcSeries)
  if (selected.has(key)) {
    selected.delete(key)
  } else {
    selected.add(key)
  }
  historySelection.adcSeries = adcSeriesOptions.value.filter((option) => selected.has(option.key)).map((option) => option.key)
}

function toggleAtmosphereStation(stationId: string) {
  const selected = new Set(configForm.atmosphere.stationIds.map((id) => id.trim()).filter(Boolean))
  if (selected.has(stationId)) {
    selected.delete(stationId)
  } else {
    selected.add(stationId)
  }
  configForm.atmosphere.stationIds = Array.from(selected)
  configDirty.value = true
}

function toggleGnssDataset(datasetId: string) {
  if (gnssSelectedIds.value.includes(datasetId)) {
    gnssSelectedIds.value = gnssSelectedIds.value.filter((item) => item !== datasetId)
  } else {
    gnssSelectedIds.value = [...gnssSelectedIds.value, datasetId]
  }
  loadGnssSeries()
}

async function loadGnssDataSets() {
  if (!deviceId.value) return
  gnssLoading.value = true
  try {
    const items = await apiFetch<GnssData[]>(`/api/devices/${deviceId.value}/gnss-data`)
    gnssDataSets.value = items || []
    const available = new Set(gnssDataSets.value.map((item) => item.id))
    const selected = gnssSelectedIds.value.filter((item) => available.has(item))
    gnssSelectedIds.value = selected.length ? selected : gnssDataSets.value.map((item) => item.id)
  } catch (e: any) {
    gnssStatus.value = e?.data?.detail || e?.message || 'Не удалось загрузить GNSS источники'
  } finally {
    gnssLoading.value = false
  }
}

async function loadGnssSeries() {
  if (!deviceId.value || !historyFilters.from || !historyFilters.to || !gnssSelectedIds.value.length) {
    gnssSeries.value = []
    return
  }
  const from = localInputToIso(historyFilters.from)
  const to = historyAutoRefresh.value ? new Date().toISOString() : localInputToIso(historyFilters.to)
  if (!from || !to) return
  try {
    const params = new URLSearchParams({ from, to, limit_per_dataset: '10000' })
    gnssSelectedIds.value.forEach((id) => params.append('ids', id))
    const response = await apiFetch<GnssDataSeriesResponse>(
      `/api/devices/${deviceId.value}/gnss-data/series?${params.toString()}`,
    )
    gnssSeries.value = response.items || []
    const capped = gnssSeries.value.filter((item) => item.capped).map((item) => item.dataset.name)
    if (capped.length) {
      gnssStatus.value = `GNSS ряд обрезан лимитом: ${capped.join(', ')}`
    }
  } catch (e: any) {
    gnssSeries.value = []
    gnssStatus.value = e?.data?.detail || e?.message || 'Не удалось загрузить GNSS ряд'
  }
}

async function loadStationOptions(query = stationSearchQuery.value) {
  stationOptionsLoading.value = true
  try {
    const params = new URLSearchParams({ limit: '80', offset: '0' })
    const trimmed = query.trim()
    if (trimmed) params.set('query', trimmed)
    const res = await apiFetch<{ items: StationOption[]; total: number }>(`/api/stations?${params.toString()}`)
    stationOptions.value = (res.items || []).map((item) => ({ station_id: item.station_id, name: item.name || null }))
  } catch (e) {
    stationOptions.value = []
  } finally {
    stationOptionsLoading.value = false
  }
}

async function loadAtmosphere() {
  if (!deviceId.value) return
  if (!historyFilters.from) return
  const selected = deviceConfig.value?.atmosphere_config?.station_ids || []
  if (!selected.length) {
    atmosphereData.value = null
    atmosphereStatus.value = 'Выберите станции профилей в настройках устройства'
    return
  }
  try {
    const params = new URLSearchParams({
      limit: String(historyFilters.limit),
      average: atmosphereAverage.value ? 'true' : 'false',
    })
    appendTempOutlierFilterParams(params)
    params.set('coefficients', JSON.stringify(buildAtmosphereCoefficientPayload()))
    const primary = atmospherePrimaryStation.value || selected[0]
    if (primary) params.set('tau_station_id', primary)
    if (historyFilters.bucketMode === 'manual') {
      const multiplier = historyFilters.bucketUnit === 'h' ? 3600 : historyFilters.bucketUnit === 'm' ? 60 : 1
      const seconds = Math.max(1, Math.floor(historyFilters.bucketValue * multiplier))
      params.set('bucket_seconds', String(seconds))
    }
    if (historyFilters.from) {
      const isoFrom = localInputToIso(historyFilters.from)
      if (isoFrom) params.set('from', isoFrom)
    }
    if (historyAutoRefresh.value) {
      params.set('to', new Date().toISOString())
    } else if (historyFilters.to) {
      const isoTo = localInputToIso(historyFilters.to)
      if (isoTo) params.set('to', isoTo)
    }
    const response = await apiFetch<AtmosphereResponse>(`/api/devices/${deviceId.value}/atmosphere?${params.toString()}`)
    atmosphereData.value = response
    atmosphereStatus.value = ''
  } catch (e: any) {
    atmosphereStatus.value = e?.data?.detail || e?.message || 'Не удалось рассчитать атмосферные параметры'
    atmosphereData.value = null
  }
}

async function loadAtmosphereCoefficientDefaults() {
  if (!deviceId.value) return
  try {
    const params = new URLSearchParams()
    params.set('coefficients', JSON.stringify(buildAtmosphereCoefficientPayload()))
    const response = await apiFetch<AtmosphereCoefficientDefaults>(
      `/api/devices/${deviceId.value}/atmosphere/coefficients?${params.toString()}`,
    )
    atmosphereCoefficientDefaults.value = response
  } catch (e) {
    atmosphereCoefficientDefaults.value = null
  }
}

async function loadHistory() {
  if (!deviceId.value) return
  historyLoading.value = true
  historyStatus.value = ''
  try {
    if (!validateHistoryDate('from')) {
      historyLoading.value = false
      return
    }
    if (!historyAutoRefresh.value) {
      if (!validateHistoryDate('to')) {
        historyLoading.value = false
        return
      }
    }
    const params = new URLSearchParams({
      device_id: deviceId.value,
      limit: String(historyFilters.limit),
    })
    appendTempOutlierFilterParams(params)
    if (historyFilters.bucketMode === 'manual') {
      const multiplier = historyFilters.bucketUnit === 'h' ? 3600 : historyFilters.bucketUnit === 'm' ? 60 : 1
      const seconds = Math.max(1, Math.floor(historyFilters.bucketValue * multiplier))
      params.set('bucket_seconds', String(seconds))
    }
    if (historyFilters.from) {
      const isoFrom = localInputToIso(historyFilters.from)
      if (isoFrom) params.set('from', isoFrom)
    }
    if (historyAutoRefresh.value) {
      params.set('to', new Date().toISOString())
    } else if (historyFilters.to) {
      const isoTo = localInputToIso(historyFilters.to)
      if (isoTo) params.set('to', isoTo)
    }
    const response = await apiFetch<MeasurementsResponse>(`/api/measurements?${params.toString()}`)
    historyData.value = response.points
    historyTempLabels.value = response.temp_labels || []
    historyTempAddresses.value = response.temp_addresses || []
    historyTempBindings.value = response.temp_bindings || {}
    historyBrightnessLabels.value = response.brightness_temp_labels || {}
    historyOutlierFilter.value = response.temp_outlier_filter || null
    historyBucketLabel.value = response.bucket_label
    historyRawCount.value = response.raw_count
    if (response.aggregated) {
      historyStatus.value = `Получено ${response.points.length} из ${response.raw_count} (окно ${response.bucket_label})`
    } else {
      historyStatus.value = `Получено ${response.points.length} точек`
    }
    await loadAtmosphere()
    await loadGnssSeries()
  } catch (e: any) {
    historyStatus.value = e?.message || 'Не удалось загрузить историю'
  } finally {
    historyLoading.value = false
  }
}

const startHistoryTimer = () => {
  if (historyTimer) clearInterval(historyTimer)
  historyTimer = setInterval(() => {
    if (historyLoading.value) return
    if (historyDateEditing.from || historyDateEditing.to || historyDateError.value) return
    loadHistory()
  }, Math.max(5, historyRefreshSec.value) * 1000)
}

const stopHistoryTimer = () => {
  if (historyTimer) {
    clearInterval(historyTimer)
    historyTimer = null
  }
}

const seedConfigForm = () => {
  const config = deviceConfig.value
  if (!config) return
  const atmosphere = config.atmosphere_config || {}
  configForm.atmosphere.stationIds = [...(atmosphere.station_ids || [])]
  configForm.atmosphere.altitudeM = Number.isFinite(Number(atmosphere.altitude_m)) ? Number(atmosphere.altitude_m) : 0
  configForm.atmosphere.h0M = Number.isFinite(Number(atmosphere.h0_m)) ? Number(atmosphere.h0_m) : 5300
  configForm.atmosphere.season = normalizedSeason(atmosphere.season)
  const bands = atmosphere.bands || {}
  atmosphereBandKeys.forEach((key) => {
    const defaultMode = key === 'adc2' ? '2mm' : '3mm'
    const band = (bands[key] || {}) as AtmosphereBandConfig
    configForm.atmosphere.bands[key] = {
      mode: normalizedBandMode(band.mode, defaultMode),
      alpha: finiteOrNull(band.alpha),
      beta: finiteOrNull(band.beta),
    }
  })
  atmospherePrimaryStation.value = String(atmosphere.tau_station_id || configForm.atmosphere.stationIds[0] || '')
  atmosphereAverage.value = !!atmosphere.tau_average
}

const resetConfig = () => {
  configDirty.value = false
  configStatus.value = ''
  seedConfigForm()
}

const saveConfig = async () => {
  if (!deviceId.value) return
  configSaving.value = true
  configStatus.value = 'Сохраняю коэффициенты...'
  try {
    const stationIds = configForm.atmosphere.stationIds.map((id) => id.trim()).filter(Boolean)
    const atmosphereConfig = {
      ...(props.config.atmosphere_config || {}),
      station_ids: stationIds,
      altitude_m: Math.max(0, Number(configForm.atmosphere.altitudeM) || 0),
      h0_m: Math.max(1, Number(configForm.atmosphere.h0M) || 5300),
      tau_station_id: stationIds.includes(atmospherePrimaryStation.value)
        ? atmospherePrimaryStation.value
        : (stationIds[0] || ''),
      tau_average: atmosphereAverage.value,
      ...buildAtmosphereCoefficientPayload(),
    }
    const saved = await apiFetch<DeviceConfig>(`/api/devices/${deviceId.value}`, {
      method: 'PATCH',
      body: { atmosphere_config: atmosphereConfig },
    })
    configDirty.value = false
    configStatus.value = 'Коэффициенты сохранены'
    emit('config-updated', saved)
    await loadAtmosphereCoefficientDefaults()
    await loadAtmosphere()
  } catch (e: any) {
    configStatus.value = e?.data?.detail || e?.message || 'Не удалось сохранить коэффициенты'
  } finally {
    configSaving.value = false
  }
}

const initialize = async () => {
  setHistoryWindow(new Date())
  seedConfigForm()
  loadStationOptions()
  loadAtmosphereCoefficientDefaults()
  await loadGnssDataSets()
  await loadHistory()
}

onBeforeMount(() => {
  if (!historyFilters.from || !historyFilters.to) setHistoryWindow(new Date())
})

onMounted(() => {
  void initialize()
  browserTzLabel.value = browserTimezoneLabel()
})

onBeforeUnmount(() => {
  stopHistoryTimer()
  if (stationSearchTimer) clearTimeout(stationSearchTimer)
  if (atmosphereReloadTimer) clearTimeout(atmosphereReloadTimer)
})

onDeactivated(stopHistoryTimer)
onActivated(() => {
  if (!historyFilters.from || !historyFilters.to) setHistoryWindow(new Date())
  if (historyAutoRefresh.value) startHistoryTimer()
})

watch(
  () => props.deviceId,
  () => {
    stopHistoryTimer()
    historyData.value = []
    atmosphereData.value = null
    gnssDataSets.value = []
    gnssSeries.value = []
    configDirty.value = false
    initialize()
  },
)

watch(
  () => props.config,
  () => {
    if (!configDirty.value) seedConfigForm()
  },
  { deep: true },
)

watch(
  () => props.gnssRevision,
  async () => {
    await loadGnssDataSets()
    await loadGnssSeries()
  },
)

</script>
