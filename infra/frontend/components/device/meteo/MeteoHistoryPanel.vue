<template>
  <div class="card meteo-history-panel">
    <div class="card-head">
      <h3>История метеоданных</h3>
      <span class="badge">Графики</span>
    </div>
    <div class="inline fields">
      <label class="compact">С
        <input type="datetime-local" class="date-input" v-model="filters.from" />
      </label>
      <label class="compact">По
        <input type="datetime-local" class="date-input" v-model="filters.to" :disabled="autoRefresh" />
      </label>
      <label class="compact">Лимит
        <input type="number" min="100" max="10000" step="100" v-model.number="filters.limit" />
      </label>
      <label class="compact">Усреднение
        <select v-model="filters.bucketMode">
          <option value="auto">Авто</option>
          <option value="manual">Вручную</option>
        </select>
      </label>
      <label class="compact" v-if="filters.bucketMode === 'manual'">Окно
        <span class="inline">
          <input type="number" min="1" step="1" v-model.number="filters.bucketValue" />
          <select v-model="filters.bucketUnit">
            <option value="s">сек</option>
            <option value="m">мин</option>
            <option value="h">ч</option>
          </select>
        </span>
      </label>
    </div>
    <p class="muted" v-if="timezoneLabel">Таймзона браузера: {{ timezoneLabel }}</p>
    <div class="actions">
      <button class="btn primary" type="button" @click="load" :disabled="loading">Загрузить</button>
      <label class="checkbox"><input type="checkbox" v-model="autoRefresh" /><span>Автообновление</span></label>
      <label class="compact" v-if="autoRefresh">Интервал, сек
        <input type="number" min="5" max="300" step="5" v-model.number="refreshSeconds" />
      </label>
      <span class="muted" v-if="status">{{ status }}</span>
    </div>
    <div class="status-row" v-if="bucketLabel && bucketLabel !== 'raw'">
      <span class="chip subtle">Окно: {{ bucketLabel }}</span>
      <span class="chip subtle">Исходно: {{ rawCount }}</span>
    </div>
    <p class="muted" v-if="loading">Загрузка...</p>
    <p class="muted" v-else-if="loaded && !hasAnyData">В выбранном диапазоне метеоданных нет.</p>

    <div v-if="hasAnyData" class="chart-stack meteo-chart-stack">
      <div v-if="hasTemperatureHumidity" class="chart-box compact-chart-box"><h4>Температура и влажность</h4><div class="chart-body"><canvas ref="temperatureHumidityEl"></canvas></div></div>
      <div v-if="hasPressure" class="chart-box compact-chart-box"><h4>Давление</h4><div class="chart-body"><canvas ref="pressureEl"></canvas></div></div>
      <div v-if="hasWind" class="chart-box compact-chart-box"><h4>Ветер и порывы</h4><div class="chart-body"><canvas ref="windEl"></canvas></div></div>
      <div v-if="hasDirection" class="chart-box compact-chart-box"><h4>Направление ветра</h4><div class="chart-body"><canvas ref="directionEl"></canvas></div></div>
      <div v-if="hasRainfall" class="chart-box compact-chart-box"><h4>Осадки</h4><div class="chart-body"><canvas ref="rainfallEl"></canvas></div></div>
      <div v-if="hasLightUvi" class="chart-box compact-chart-box"><h4>Освещённость и УФ-индекс</h4><div class="chart-body"><canvas ref="lightUviEl"></canvas></div></div>
    </div>
  </div>
</template>

<script setup lang="ts">
import type { MeteoHistoryPoint, MeteoHistoryResponse } from '~/types/device'
import { browserTimezoneLabel, formatChartTimestamp, localInputToIso, toLocalInputValue } from '~/utils/datetime'

const props = defineProps<{ deviceId: string }>()
const { apiFetch } = useApi()

const filters = reactive({ from: '', to: '', limit: 2000, bucketMode: 'auto', bucketValue: 1, bucketUnit: 'm' })
const points = ref<MeteoHistoryPoint[]>([])
const loading = ref(false)
const loaded = ref(false)
const status = ref('')
const rawCount = ref(0)
const bucketLabel = ref('')
const autoRefresh = ref(false)
const refreshSeconds = ref(30)
const timezoneLabel = ref('')
let refreshTimer: ReturnType<typeof setInterval> | null = null
let requestVersion = 0
let ChartCtor: any = null

const temperatureHumidityEl = ref<HTMLCanvasElement | null>(null)
const pressureEl = ref<HTMLCanvasElement | null>(null)
const windEl = ref<HTMLCanvasElement | null>(null)
const directionEl = ref<HTMLCanvasElement | null>(null)
const rainfallEl = ref<HTMLCanvasElement | null>(null)
const lightUviEl = ref<HTMLCanvasElement | null>(null)
const charts: Record<string, any> = {}

const finite = (value: unknown) => Number.isFinite(Number(value))
const hasField = (key: keyof MeteoHistoryPoint) => points.value.some((point) => point[key] !== null && finite(point[key]))
const hasTemperatureHumidity = computed(() => hasField('temp_c') || hasField('humidity_pct'))
const hasPressure = computed(() => hasField('pressure_hpa'))
const hasWind = computed(() => hasField('wind_speed_ms') || hasField('gust_speed_ms'))
const hasDirection = computed(() => hasField('wind_dir_deg'))
const hasRainfall = computed(() => hasField('rainfall_mm'))
const hasLightUvi = computed(() => hasField('light_lux') || hasField('uvi'))
const hasAnyData = computed(() => hasTemperatureHumidity.value || hasPressure.value || hasWind.value || hasDirection.value || hasRainfall.value || hasLightUvi.value)

const labels = () => points.value.map((point) => formatChartTimestamp(point.timestamp))
const values = (key: keyof MeteoHistoryPoint) => points.value.map((point) => point[key] !== null && finite(point[key]) ? Number(point[key]) : null)
const dataset = (label: string, key: keyof MeteoHistoryPoint, color: string, axis = 'y') => ({
  label, data: values(key), borderColor: color, backgroundColor: color, borderWidth: 2,
  pointRadius: 0, tension: 0.2, yAxisID: axis,
})
const axis = (title: string, position: 'left' | 'right' = 'left', extra: Record<string, any> = {}) => ({
  type: 'linear', position, title: { display: true, text: title }, ticks: { maxTicksLimit: 6 }, ...extra,
})
const options = (scales: Record<string, any>) => ({
  responsive: true, maintainAspectRatio: false, animation: false, normalized: true,
  interaction: { mode: 'index', intersect: false },
  plugins: { legend: { position: 'top', align: 'start', labels: { usePointStyle: true, boxWidth: 10 } } },
  scales: { x: { ticks: { maxTicksLimit: 6, autoSkip: true, maxRotation: 0 }, grid: { display: false } }, ...scales },
})

const upsertChart = (name: string, element: HTMLCanvasElement | null, datasets: any[], scales: Record<string, any>) => {
  if (!ChartCtor || !element) return
  if (!charts[name]) {
    charts[name] = new ChartCtor(element, { type: 'line', data: { labels: labels(), datasets }, options: options(scales) })
  } else {
    const hidden = new Map<string, boolean>()
    charts[name].data.datasets.forEach((item: any, index: number) => {
      if (item?.label) hidden.set(item.label, !charts[name].isDatasetVisible(index))
    })
    datasets.forEach((item) => {
      if (hidden.has(item.label)) item.hidden = hidden.get(item.label)
    })
    charts[name].data.labels = labels()
    charts[name].data.datasets = datasets
    charts[name].update('none')
  }
}

const destroyMissingCharts = () => {
  const visible: Record<string, boolean> = {
    temperatureHumidity: hasTemperatureHumidity.value, pressure: hasPressure.value, wind: hasWind.value,
    direction: hasDirection.value, rainfall: hasRainfall.value, lightUvi: hasLightUvi.value,
  }
  Object.entries(charts).forEach(([name, chart]) => {
    if (!visible[name]) { chart.destroy(); delete charts[name] }
  })
}

const render = async () => {
  if (!ChartCtor) return
  await nextTick()
  destroyMissingCharts()
  if (hasTemperatureHumidity.value) upsertChart('temperatureHumidity', temperatureHumidityEl.value, [
    dataset('Температура, °C', 'temp_c', '#d62728', 'y'), dataset('Влажность, %', 'humidity_pct', '#1f77b4', 'y1'),
  ], { y: axis('°C'), y1: axis('%', 'right', { grid: { drawOnChartArea: false }, min: 0, max: 100 }) })
  if (hasPressure.value) upsertChart('pressure', pressureEl.value, [dataset('Давление, hPa', 'pressure_hpa', '#9467bd')], { y: axis('hPa') })
  if (hasWind.value) upsertChart('wind', windEl.value, [dataset('Ветер', 'wind_speed_ms', '#2ca02c'), dataset('Порывы', 'gust_speed_ms', '#ff7f0e')], { y: axis('м/с', 'left', { beginAtZero: true }) })
  if (hasDirection.value) upsertChart('direction', directionEl.value, [dataset('Направление', 'wind_dir_deg', '#17becf')], { y: axis('°', 'left', { min: 0, max: 360, ticks: { stepSize: 90 } }) })
  if (hasRainfall.value) upsertChart('rainfall', rainfallEl.value, [dataset('Осадки', 'rainfall_mm', '#1f77b4')], { y: axis('мм', 'left', { beginAtZero: true }) })
  if (hasLightUvi.value) upsertChart('lightUvi', lightUviEl.value, [dataset('Освещённость', 'light_lux', '#bcbd22', 'y'), dataset('УФ-индекс', 'uvi', '#e377c2', 'y1')], { y: axis('лк'), y1: axis('UVI', 'right', { grid: { drawOnChartArea: false }, beginAtZero: true }) })
}

const buildParams = () => {
  const params = new URLSearchParams({ device_id: props.deviceId, limit: String(Math.max(100, Math.min(10000, filters.limit || 2000))) })
  const from = localInputToIso(filters.from)
  const to = autoRefresh.value ? new Date().toISOString() : localInputToIso(filters.to)
  if (from) params.set('from', from)
  if (to) params.set('to', to)
  if (filters.bucketMode === 'manual') {
    const multiplier = filters.bucketUnit === 'h' ? 3600 : filters.bucketUnit === 'm' ? 60 : 1
    params.set('bucket_seconds', String(Math.max(1, Math.min(86400, Math.floor(filters.bucketValue * multiplier)))))
  }
  return params
}

const load = async () => {
  if (!props.deviceId) return
  const from = localInputToIso(filters.from)
  const to = autoRefresh.value ? new Date().toISOString() : localInputToIso(filters.to)
  if (!from || !to || new Date(from).getTime() > new Date(to).getTime()) {
    status.value = 'Проверьте диапазон дат: начало должно быть не позже конца'
    return
  }
  const version = ++requestVersion
  loading.value = true
  status.value = ''
  try {
    const response = await apiFetch<MeteoHistoryResponse>(`/api/meteo-readings?${buildParams()}`)
    if (version !== requestVersion) return
    points.value = response.points || []
    rawCount.value = response.raw_count
    bucketLabel.value = response.bucket_label
    loaded.value = true
    status.value = response.aggregated
      ? `Получено ${response.points.length} из ${response.raw_count} (окно ${response.bucket_label})`
      : `Получено ${response.points.length} точек`
    await render()
  } catch (error: any) {
    if (version === requestVersion) status.value = error?.message || 'Не удалось загрузить историю метео'
  } finally {
    if (version === requestVersion) loading.value = false
  }
}

const stopTimer = () => { if (refreshTimer) clearInterval(refreshTimer); refreshTimer = null }
const startTimer = () => {
  stopTimer()
  if (!autoRefresh.value) return
  refreshTimer = setInterval(load, Math.max(5, Math.min(300, refreshSeconds.value)) * 1000)
}
const destroyCharts = () => { Object.keys(charts).forEach((key) => { charts[key].destroy(); delete charts[key] }) }

watch([autoRefresh, refreshSeconds], startTimer)
watch(() => props.deviceId, () => {
  requestVersion += 1
  points.value = []
  loaded.value = false
  status.value = ''
  destroyCharts()
  load()
})

onMounted(async () => {
  const end = new Date()
  filters.to = toLocalInputValue(end)
  filters.from = toLocalInputValue(new Date(end.getTime() - 24 * 60 * 60 * 1000))
  timezoneLabel.value = browserTimezoneLabel()
  const module: any = await import('chart.js/auto')
  ChartCtor = module?.Chart || module?.default || module
  await load()
})
onBeforeUnmount(() => { requestVersion += 1; stopTimer(); destroyCharts() })
onDeactivated(stopTimer)
onActivated(startTimer)
</script>
