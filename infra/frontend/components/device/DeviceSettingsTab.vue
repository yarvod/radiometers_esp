<template>
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
</template>

<script setup lang="ts">
import type { DeviceConfig } from '~/types/device'

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
let searchTimer: ReturnType<typeof setTimeout> | null = null

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
watch(() => props.deviceId, () => { seed(); loadStations() })
watch(stationQuery, () => { if (searchTimer) clearTimeout(searchTimer); searchTimer = setTimeout(loadStations, 300) })
onMounted(() => { seed(); loadStations() })
onBeforeUnmount(() => { if (searchTimer) clearTimeout(searchTimer) })
</script>
