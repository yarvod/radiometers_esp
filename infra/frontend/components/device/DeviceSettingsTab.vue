<template>
  <div class="settings-stack">
  <div class="card">
    <div class="card-head"><h3>Настройки устройства</h3><span class="badge">Конфиг</span></div>
    <p class="muted" v-if="status">{{ status }}</p>
    <div class="form-group"><label>Название устройства</label><input type="text" v-model="form.displayName" @input="dirty = true" /></div>
    <div class="form-group">
      <label>Температурные датчики</label>
      <div class="config-table">
        <div class="config-row header"><span>Индекс</span><span>Адрес</span><span>Имя</span></div>
        <div v-if="form.tempRows.length === 0" class="muted">Нет данных о датчиках</div>
        <div v-for="row in form.tempRows" :key="`${row.index}-${row.address}`" class="config-row">
          <span class="chip subtle">t{{ row.index + 1 }}</span><span class="muted small">{{ row.address || '—' }}</span>
          <input type="text" v-model="row.label" @input="dirty = true" />
        </div>
      </div>
    </div>
    <div class="form-group">
      <label>ADC / Cal</label>
      <div class="config-grid">
        <label class="compact" v-for="key in adcKeys" :key="key">{{ adcDefaults[key] }}
          <input type="text" v-model="form.adcLabels[key]" @input="dirty = true" />
        </label>
      </div>
    </div>
    <div class="form-group">
      <label>Привязка температур</label>
      <div class="config-binding-list">
        <label class="config-binding-row" v-for="key in bindingKeys" :key="key">
          <span>{{ bindingLabel(key) }}</span>
          <select v-model="form.tempBindings[key]" @change="dirty = true">
            <option value="">Не задано</option>
            <option v-for="sensor in sensors" :key="`${key}-${sensor.address}`" :value="sensor.address">{{ sensor.label }} ({{ sensor.address }})</option>
          </select>
        </label>
      </div>
    </div>
    <div class="form-group">
      <label>Атмосферные профили для расчета Tэфф / tau / PWV</label>
      <div class="config-grid">
        <label class="compact">Высота прибора, м<input type="number" min="0" step="1" v-model.number="form.altitudeM" @input="dirty = true" /></label>
        <label class="compact">h0, м<input type="number" min="1" step="1" v-model.number="form.h0M" @input="dirty = true" /></label>
      </div>
      <div class="station-multiselect">
        <button class="station-multiselect-head" type="button" @click="stationOpen = !stationOpen">
          <span v-if="selectedStations.length === 0" class="muted">Выбрать станции</span>
          <span v-for="station in selectedStations" :key="station.station_id" class="station-token">{{ stationLabel(station) }}<span class="station-token-remove" @click.stop="toggleStation(station.station_id)">×</span></span>
          <span class="station-count">{{ selectedStations.length }}</span><span class="station-caret">⌄</span>
        </button>
        <div v-if="stationOpen" class="station-multiselect-menu">
          <input class="station-search-input" type="text" v-model="stationQuery" placeholder="Поиск станции по ID или названию..." />
          <div class="station-option-list">
            <button v-for="station in stations" :key="station.station_id" type="button" class="station-option-row" :class="{ selected: form.stationIds.includes(station.station_id) }" @click="toggleStation(station.station_id)">
              <span class="station-check">{{ form.stationIds.includes(station.station_id) ? '✓' : '' }}</span><span>{{ stationLabel(station) }}</span>
            </button>
            <div v-if="stations.length === 0" class="muted station-empty">{{ stationsLoading ? 'Ищем станции...' : 'Станции не найдены' }}</div>
          </div>
        </div>
      </div>
      <p class="muted small">Профили с количеством точек меньше 50 в расчет не попадают.</p>
    </div>
    <div class="actions"><button class="btn primary" @click="save" :disabled="saving">Сохранить</button><button class="btn ghost" @click="seed" :disabled="saving">Сбросить</button></div>
  </div>

  <div class="card s3-card">
    <div class="card-head">
      <div>
        <h3>Восстановление данных из MinIO</h3>
        <p class="muted small s3-intro">Дополняет базу измерениями из файлов, которые устройство сохранило без интернета.</p>
      </div>
      <span class="badge" :class="{ success: s3Config?.enabled, accent: s3Config?.running }">
        {{ s3Config?.running ? 'Выполняется' : s3Config?.enabled ? 'Включено' : 'Выключено' }}
      </span>
    </div>

    <div v-if="s3Loading && !s3Config" class="muted">Загружаю настройки синхронизации…</div>
    <template v-else>
      <label class="checkbox s3-toggle">
        <input v-model="s3Form.enabled" type="checkbox" @change="s3Dirty = true" />
        <span>
          <strong>Автоматически проверять файлы устройства</strong>
          <small>ARQ-воркер будет искать новые файлы с заданным интервалом.</small>
        </span>
      </label>

      <div class="s3-fields">
        <label class="form-group">
          <span>Bucket устройства</span>
          <input v-model.trim="s3Form.bucket" type="text" :placeholder="deviceId" @input="s3Dirty = true" />
          <small class="muted">Только имя bucket, без адреса MinIO и слешей.</small>
        </label>
        <label class="form-group">
          <span>Проверять каждые, минут</span>
          <input v-model.number="s3Form.intervalMinutes" type="number" min="1" max="10080" step="1" @input="s3Dirty = true" />
        </label>
      </div>

      <details class="s3-advanced">
        <summary>Дополнительные настройки</summary>
        <div class="s3-fields">
          <label class="form-group">
            <span>Папка радиометра</span>
            <input v-model.trim="s3Form.radiometerPrefix" type="text" placeholder="radiometers/" @input="s3Dirty = true" />
          </label>
          <label class="form-group">
            <span>Папка метеостанции</span>
            <input v-model.trim="s3Form.meteoPrefix" type="text" placeholder="meteo/" @input="s3Dirty = true" />
          </label>
          <label class="form-group">
            <span>Файлов каждой папки за один запуск</span>
            <input v-model.number="s3Form.maxFilesPerPrefix" type="number" min="1" max="100" step="1" @input="s3Dirty = true" />
          </label>
        </div>
      </details>

      <div v-if="s3Config" class="s3-stats">
        <div><span>Последний успешный запуск</span><strong>{{ formatSyncDate(s3Config.last_success_at) }}</strong></div>
        <div><span>Следующая проверка</span><strong>{{ s3Config.enabled ? formatSyncDate(s3Config.next_run_at) : 'Автосинхронизация выключена' }}</strong></div>
        <div><span>Обработано файлов</span><strong>{{ s3Config.processed_files }}</strong></div>
        <div><span>Восстановлено измерений</span><strong>{{ s3Config.inserted_measurements }}</strong></div>
        <div><span>Восстановлено метеозаписей</span><strong>{{ s3Config.inserted_meteo_readings }}</strong></div>
      </div>

      <div v-if="s3Config?.last_radiometer_key || s3Config?.last_meteo_key" class="s3-cursors">
        <div v-if="s3Config.last_radiometer_key"><span>Последний файл радиометра</span><code>{{ s3Config.last_radiometer_key }}</code></div>
        <div v-if="s3Config.last_meteo_key"><span>Последний файл метеостанции</span><code>{{ s3Config.last_meteo_key }}</code></div>
      </div>

      <div v-if="s3Config?.last_error" class="s3-message error"><strong>Последняя ошибка</strong><span>{{ s3Config.last_error }}</span></div>
      <div v-else-if="s3Status" class="s3-message" :class="{ error: s3StatusError }">{{ s3Status }}</div>

      <div class="actions s3-actions">
        <button class="btn primary" type="button" :disabled="s3Saving || s3Loading" @click="saveS3Sync">
          {{ s3Saving ? 'Сохраняю…' : 'Сохранить синхронизацию' }}
        </button>
        <button class="btn ghost" type="button" :disabled="s3Saving || s3Running || s3Loading || s3Config?.running" @click="runS3SyncNow">
          {{ s3Running ? 'Запускаю…' : 'Проверить сейчас' }}
        </button>
        <button class="btn ghost" type="button" :disabled="s3Loading" @click="loadS3Sync">Обновить статус</button>
      </div>
      <p class="muted small">«Проверить сейчас» работает даже при выключенной автоматической синхронизации.</p>
    </template>
  </div>
  </div>
</template>

<script setup lang="ts">
import type { DeviceConfig, DeviceS3SyncConfig, DeviceS3SyncUpdate } from '~/types/device'

type TempRow = { index: number; address: string; label: string }
type Station = { station_id: string; name: string | null }
type LiveTemp = { address?: string; label: string }

const props = defineProps<{ deviceId: string; config: DeviceConfig; liveTemps: LiveTemp[] }>()
const emit = defineEmits<{ 'config-updated': [config: DeviceConfig] }>()
const { apiFetch } = useApi()
const adcDefaults: Record<string, string> = { adc1: 'ADC1', adc2: 'ADC2', adc3: 'ADC3', adc1_cal: 'ADC1 Cal', adc2_cal: 'ADC2 Cal', adc3_cal: 'ADC3 Cal' }
const adcKeys = Object.keys(adcDefaults)
const bindingKeys = ['radiometer_adc1', 'radiometer_adc2', 'radiometer_adc3', 'calibration_load']
const form = reactive({ displayName: '', tempRows: [] as TempRow[], adcLabels: {} as Record<string, string>, tempBindings: {} as Record<string, string>, stationIds: [] as string[], altitudeM: 0, h0M: 5300 })
const dirty = ref(false)
const saving = ref(false)
const status = ref('')
const stationOpen = ref(false)
const stationQuery = ref('')
const stations = ref<Station[]>([])
const stationsLoading = ref(false)
const s3Config = ref<DeviceS3SyncConfig | null>(null)
const s3Form = reactive({
  enabled: false,
  bucket: props.deviceId,
  intervalMinutes: 10,
  radiometerPrefix: 'radiometers/',
  meteoPrefix: 'meteo/',
  maxFilesPerPrefix: 10,
})
const s3Loading = ref(false)
const s3Saving = ref(false)
const s3Running = ref(false)
const s3Dirty = ref(false)
const s3Status = ref('')
const s3StatusError = ref(false)
let searchTimer: ReturnType<typeof setTimeout> | null = null
let syncPollTimer: ReturnType<typeof setTimeout> | null = null

const labelMap = () => {
  const map = new Map(Object.entries(props.config.temp_label_map || {}))
  props.config.temp_addresses.forEach((address, index) => { if (address && !map.has(address)) map.set(address, props.config.temp_labels[index] || `t${index + 1}`) })
  return map
}
const sensors = computed(() => form.tempRows.filter((row) => row.address).map((row) => ({ address: row.address, label: row.label || `t${row.index + 1}` })))
const selectedStations = computed(() => {
  const byId = new Map(stations.value.map((station) => [station.station_id, station]))
  return form.stationIds.map((id) => byId.get(id) || { station_id: id, name: null })
})

const seed = () => {
  const map = labelMap()
  const length = Math.max(props.config.temp_labels.length, props.config.temp_addresses.length, props.liveTemps.length)
  form.displayName = props.config.display_name || ''
  form.tempRows = Array.from({ length }, (_, index) => {
    const live = props.liveTemps[index]
    const address = props.config.temp_addresses[index] || live?.address || ''
    return { index, address, label: (address && map.get(address)) || props.config.temp_labels[index] || live?.label || `t${index + 1}` }
  })
  form.adcLabels = { ...adcDefaults, ...(props.config.adc_labels || {}) }
  form.tempBindings = Object.fromEntries(bindingKeys.map((key) => [key, props.config.temp_bindings?.[key] || '']))
  const atmosphere = props.config.atmosphere_config || {}
  form.stationIds = [...(atmosphere.station_ids || [])]
  form.altitudeM = Number(atmosphere.altitude_m || 0)
  form.h0M = Number(atmosphere.h0_m || 5300)
  dirty.value = false
  status.value = ''
}

const loadStations = async () => {
  stationsLoading.value = true
  try {
    const params = new URLSearchParams({ limit: '80', offset: '0' })
    if (stationQuery.value.trim()) params.set('query', stationQuery.value.trim())
    const response = await apiFetch<{ items: Station[] }>(`/api/stations?${params}`)
    stations.value = response.items || []
  } catch { stations.value = [] } finally { stationsLoading.value = false }
}

const toggleStation = (id: string) => {
  form.stationIds = form.stationIds.includes(id) ? form.stationIds.filter((item) => item !== id) : [...form.stationIds, id]
  dirty.value = true
}
const stationLabel = (station: Station) => station.name ? `${station.station_id} · ${station.name}` : station.station_id
const bindingLabel = (key: string) => key === 'calibration_load' ? 'Теплая калибровочная нагрузка' : `Температура ${form.adcLabels[key.replace('radiometer_', '')] || key}`

const save = async () => {
  saving.value = true
  status.value = 'Сохраняю настройки...'
  try {
    const tempLabels = form.tempRows.map((row) => row.label.trim() || `t${row.index + 1}`)
    const tempAddresses = form.tempRows.map((row) => row.address.trim())
    const tempLabelMap = Object.fromEntries(form.tempRows.filter((row) => row.address).map((row) => [row.address.trim(), row.label.trim() || `t${row.index + 1}`]))
    const adcLabels = Object.fromEntries(adcKeys.map((key) => [key, form.adcLabels[key]?.trim() || adcDefaults[key]]))
    const tempBindings = Object.fromEntries(bindingKeys.map((key) => [key, form.tempBindings[key]?.trim()]).filter(([, value]) => value))
    const previousAtmosphere = props.config.atmosphere_config || {}
    const stationIds = form.stationIds.map((id) => id.trim()).filter(Boolean)
    const atmosphereConfig = {
      ...previousAtmosphere,
      station_ids: stationIds,
      altitude_m: Math.max(0, Number(form.altitudeM) || 0),
      h0_m: Math.max(1, Number(form.h0M) || 5300),
      tau_station_id: stationIds.includes(String(previousAtmosphere.tau_station_id || '')) ? previousAtmosphere.tau_station_id : (stationIds[0] || ''),
    }
    const updated = await apiFetch<DeviceConfig>(`/api/devices/${props.deviceId}`, {
      method: 'PATCH', body: { display_name: form.displayName.trim() || null, temp_labels: tempLabels, temp_addresses: tempAddresses, temp_label_map: tempLabelMap, temp_bindings: tempBindings, atmosphere_config: atmosphereConfig, adc_labels: adcLabels },
    })
    dirty.value = false
    status.value = 'Настройки сохранены'
    emit('config-updated', updated)
  } catch (error: any) { status.value = error?.message || 'Не удалось сохранить настройки' } finally { saving.value = false }
}

const seedS3Form = (config: DeviceS3SyncConfig) => {
  s3Form.enabled = config.enabled
  s3Form.bucket = config.bucket
  s3Form.intervalMinutes = config.interval_minutes
  s3Form.radiometerPrefix = config.radiometer_prefix
  s3Form.meteoPrefix = config.meteo_prefix
  s3Form.maxFilesPerPrefix = config.max_files_per_prefix
  s3Dirty.value = false
}

const syncErrorMessage = (error: any, fallback: string) => error?.data?.detail || error?.message || fallback

const loadS3Sync = async (quiet = false) => {
  if (!props.deviceId) return
  s3Loading.value = !quiet
  if (!quiet) {
    s3Status.value = ''
    s3StatusError.value = false
  }
  try {
    const config = await apiFetch<DeviceS3SyncConfig>(`/api/devices/${props.deviceId}/s3-sync`)
    s3Config.value = config
    if (!s3Dirty.value) seedS3Form(config)
  } catch (error: any) {
    s3Status.value = syncErrorMessage(error, 'Не удалось загрузить настройки синхронизации')
    s3StatusError.value = true
  } finally {
    s3Loading.value = false
  }
}

const s3Payload = (): DeviceS3SyncUpdate => ({
  enabled: s3Form.enabled,
  bucket: s3Form.bucket.trim() || props.deviceId,
  interval_minutes: Math.min(10080, Math.max(1, Number(s3Form.intervalMinutes) || 10)),
  radiometer_prefix: s3Form.radiometerPrefix.trim(),
  meteo_prefix: s3Form.meteoPrefix.trim(),
  max_files_per_prefix: Math.min(100, Math.max(1, Number(s3Form.maxFilesPerPrefix) || 10)),
})

const saveS3Sync = async (): Promise<boolean> => {
  s3Saving.value = true
  s3Status.value = 'Сохраняю настройки синхронизации…'
  s3StatusError.value = false
  try {
    const config = await apiFetch<DeviceS3SyncConfig>(`/api/devices/${props.deviceId}/s3-sync`, {
      method: 'PATCH',
      body: s3Payload(),
    })
    s3Config.value = config
    seedS3Form(config)
    s3Status.value = 'Настройки синхронизации сохранены'
    return true
  } catch (error: any) {
    s3Status.value = syncErrorMessage(error, 'Не удалось сохранить настройки синхронизации')
    s3StatusError.value = true
    return false
  } finally {
    s3Saving.value = false
  }
}

const pollS3Status = (attempt = 0) => {
  if (syncPollTimer) clearTimeout(syncPollTimer)
  if (attempt >= 10) return
  syncPollTimer = setTimeout(async () => {
    await loadS3Sync(true)
    if (s3Config.value?.running || attempt < 3) pollS3Status(attempt + 1)
  }, 2000)
}

const runS3SyncNow = async () => {
  if (s3Dirty.value && !(await saveS3Sync())) return
  s3Running.value = true
  s3Status.value = 'Ставлю проверку файлов в очередь…'
  s3StatusError.value = false
  try {
    const response = await apiFetch<{ queued: boolean }>(`/api/devices/${props.deviceId}/s3-sync/run`, { method: 'POST' })
    s3Status.value = response.queued ? 'Проверка поставлена в очередь' : 'Такая проверка уже находится в очереди'
    pollS3Status()
  } catch (error: any) {
    s3Status.value = syncErrorMessage(error, 'Не удалось запустить проверку')
    s3StatusError.value = true
  } finally {
    s3Running.value = false
  }
}

const formatSyncDate = (value: string | null) => value ? new Date(value).toLocaleString() : 'Ещё не выполнялась'

watch(() => props.config, () => { if (!dirty.value) seed() }, { deep: true })
watch(
  () => props.liveTemps.map((item, index) => `${index}:${item.address || ''}:${item.label || ''}`),
  () => {
    if (dirty.value) return
    const known = new Set(form.tempRows.map((row) => row.address).filter(Boolean))
    const hasNewAddress = props.liveTemps.some((item) => item.address && !known.has(item.address))
    if (props.liveTemps.length > form.tempRows.length || hasNewAddress) seed()
  },
)
watch(() => props.deviceId, () => { seed(); loadStations(); s3Config.value = null; s3Dirty.value = false; loadS3Sync() })
watch(stationQuery, () => { if (searchTimer) clearTimeout(searchTimer); searchTimer = setTimeout(loadStations, 300) })
onMounted(() => { seed(); loadStations() })
onActivated(() => loadS3Sync(true))
onDeactivated(() => { if (syncPollTimer) clearTimeout(syncPollTimer) })
onBeforeUnmount(() => {
  if (searchTimer) clearTimeout(searchTimer)
  if (syncPollTimer) clearTimeout(syncPollTimer)
})
</script>

<style scoped>
.settings-stack { display: grid; gap: 18px; }
.s3-card { gap: 18px; }
.s3-intro { margin: 4px 0 0; }
.s3-toggle { align-items: flex-start; padding: 14px; border: 1px solid var(--border); border-radius: 12px; background: #f8fafc; }
.s3-toggle span { display: grid; gap: 3px; }
.s3-toggle small { color: var(--muted); font-weight: 400; }
.s3-fields { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 14px; }
.s3-advanced { border: 1px solid var(--border); border-radius: 12px; padding: 12px 14px; }
.s3-advanced summary { cursor: pointer; font-weight: 700; }
.s3-advanced[open] summary { margin-bottom: 14px; }
.s3-stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 10px; }
.s3-stats > div { display: grid; gap: 4px; padding: 12px; border-radius: 10px; background: #f8fafc; }
.s3-stats span, .s3-cursors span { color: var(--muted); font-size: 12px; }
.s3-stats strong { font-size: 14px; overflow-wrap: anywhere; }
.s3-cursors { display: grid; gap: 8px; }
.s3-cursors > div { display: grid; gap: 4px; }
.s3-cursors code { overflow-wrap: anywhere; font-size: 12px; }
.s3-message { display: flex; gap: 8px; flex-wrap: wrap; padding: 12px; border-radius: 10px; background: #e8f4fd; color: #1f2d3d; }
.s3-message.error { background: #fff0ef; color: #a93226; }
.s3-actions { flex-wrap: wrap; }
@media (max-width: 640px) {
  .s3-card .card-head { align-items: flex-start; }
  .s3-fields { grid-template-columns: 1fr; }
}
</style>
