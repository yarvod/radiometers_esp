<template>
  <div v-if="deviceId" class="page device-page">
    <div class="header">
      <button class="btn ghost back" @click="navigateTo('/')">← Назад</button>
      <div>
        <div class="title">Устройство {{ deviceTitle }}</div>
        <div class="status-row">
          <span class="chip" :class="{ online: device?.online }">{{ device?.online ? 'Online' : 'Offline' }}</span>
          <span v-if="device?.lastSeen" class="chip subtle">
            Обновлено: {{ new Date(device.lastSeen).toLocaleTimeString() }}
          </span>
          <button class="btn primary sm" @click="refreshState">Обновить состояние</button>
        </div>
      </div>
    </div>

    <div class="tabs">
      <button v-for="tab in visibleTabs" :key="tab.id" class="tab-btn" :class="{ active: activeTab === tab.id }" @click="setActiveTab(tab.id)">
        {{ tab.label }}
      </button>
    </div>

    <KeepAlive :max="1">
      <DeviceDataTab
        v-if="activeTab === 'data' && deviceConfig"
        :key="`data-${deviceId}`"
        :device-id="deviceId"
        :config="deviceConfig"
        :live-temps="tempEntries"
        :gnss-revision="gnssRevision"
        @config-updated="deviceConfig = $event"
      />
    </KeepAlive>

    <KeepAlive :max="1">
      <DeviceControlTab
        v-if="activeTab === 'control'"
        :key="`control-${deviceId}`"
        :device-id="deviceId"
        :state="device?.state || {}"
        :adc-labels="adcLabelMap"
        :temps="tempEntries"
      />
    </KeepAlive>

    <KeepAlive :max="1">
      <DeviceGpsTab
        v-if="activeTab === 'gps'"
        :key="`gps-${deviceId}`"
        :device-id="deviceId"
        :state="device?.state || {}"
        @gnss-changed="gnssRevision += 1"
      />
    </KeepAlive>

    <KeepAlive :max="1">
      <DeviceMeteoTab
        v-if="activeTab === 'meteo'"
        :key="`meteo-${deviceId}`"
        :device-id="deviceId"
        :state="device?.state || {}"
        :configured="!!deviceConfig?.has_meteo"
      />
    </KeepAlive>

    <KeepAlive :max="1">
      <DeviceCalibrationPanel
        v-if="activeTab === 'calibration'"
        :key="`calibration-${deviceId}`"
        :device-id="deviceId"
        :logging="!!device?.state?.logging"
        :adc-labels="adcLabelMap"
        :temp-bindings="deviceConfig?.temp_bindings || {}"
        :temp-sensors="tempEntries"
      />
    </KeepAlive>

    <KeepAlive :max="1">
      <DeviceSettingsTab
        v-if="activeTab === 'settings' && deviceConfig"
        :key="`settings-${deviceId}`"
        :device-id="deviceId"
        :config="deviceConfig"
        :live-temps="tempEntries"
        @config-updated="deviceConfig = $event"
      />
    </KeepAlive>

    <KeepAlive :max="1">
      <DeviceErrorsTab v-if="activeTab === 'errors'" :key="`errors-${deviceId}`" :device-id="deviceId" />
    </KeepAlive>
  </div>
</template>

<script setup lang="ts">
import { useDevicesStore } from '~/stores/devices'
import type { DeviceConfig, DeviceTab } from '~/types/device'

definePageMeta({ layout: 'admin' })

const { apiFetch } = useApi()
const route = useRoute()
const store = useDevicesStore()
const nuxtApp = useNuxtApp()
store.init(nuxtApp.$mqtt)

const deviceId = computed(() => String(route.params.deviceId || ''))
const device = computed(() => (deviceId.value ? store.devices.get(deviceId.value) : undefined))
const deviceConfig = ref<DeviceConfig | null>(null)
const gnssRevision = ref(0)

const validTabs = new Set<DeviceTab>(['data', 'control', 'gps', 'meteo', 'calibration', 'settings', 'errors'])
const tabFromQuery = (): DeviceTab => {
  const raw = Array.isArray(route.query.tab) ? route.query.tab[0] : route.query.tab
  if (raw === 'gnss') return 'gps'
  return raw && validTabs.has(raw as DeviceTab) ? raw as DeviceTab : 'control'
}
const activeTab = ref<DeviceTab>(tabFromQuery())

const deviceTitle = computed(() => deviceConfig.value?.display_name || deviceId.value || '—')
const adcLabelMap = computed(() => deviceConfig.value?.adc_labels || {})
const hasMeteo = computed(() => !!deviceConfig.value?.has_meteo || device.value?.state?.meteoOnline === true)
const visibleTabs = computed<Array<{ id: DeviceTab; label: string }>>(() => [
  { id: 'data', label: 'Данные' },
  { id: 'control', label: 'Мониторинг и управление' },
  { id: 'gps', label: 'GPS' },
  ...(hasMeteo.value ? [{ id: 'meteo' as DeviceTab, label: 'Метео' }] : []),
  { id: 'calibration', label: 'Калибровка' },
  { id: 'settings', label: 'Настройки' },
  { id: 'errors', label: 'Ошибки' },
])

const buildTempLabelMap = (config: DeviceConfig | null) => {
  const result = new Map<string, string>(Object.entries(config?.temp_label_map || {}))
  ;(config?.temp_addresses || []).forEach((address, idx) => {
    if (address && !result.has(address)) result.set(address, config?.temp_labels?.[idx] || `t${idx + 1}`)
  })
  return result
}

const tempEntries = computed(() => {
  const sensors = device.value?.state?.tempSensors
  const labels = deviceConfig.value?.temp_labels || []
  const labelsByAddress = buildTempLabelMap(deviceConfig.value)
  if (Array.isArray(sensors)) {
    return sensors.map((value, idx) => ({ key: `t${idx + 1}`, label: labels[idx] || `t${idx + 1}`, value, address: '' }))
  }
  if (!sensors || typeof sensors !== 'object') return []
  return Object.entries(sensors as Record<string, any>)
    .sort(([left], [right]) => left.localeCompare(right, undefined, { numeric: true }))
    .map(([key, entry], idx) => {
      const objectEntry = entry && typeof entry === 'object'
      const address = objectEntry ? String(entry.address || '') : ''
      return {
        key,
        label: (address && labelsByAddress.get(address)) || labels[idx] || key,
        value: objectEntry ? entry.value : entry,
        address,
      }
    })
})

useHead(() => ({ title: deviceTitle.value ? `Устройство ${deviceTitle.value}` : 'Устройство' }))

const setActiveTab = (tab: DeviceTab) => {
  activeTab.value = tab
  navigateTo({ path: route.path, query: { ...route.query, tab } }, { replace: true })
}

const refreshState = async () => {
  if (!deviceId.value) return
  try {
    await store.getState(nuxtApp.$mqtt, deviceId.value)
  } catch (error) {
    console.error(error)
  }
}

const loadDeviceConfig = async () => {
  const requestedId = deviceId.value
  if (!requestedId) return
  try {
    const config = await apiFetch<DeviceConfig>(`/api/devices/${requestedId}`)
    if (deviceId.value === requestedId) deviceConfig.value = config
  } catch (error) {
    if (deviceId.value === requestedId) deviceConfig.value = null
    console.error(error)
  }
}

watch(
  () => route.query.tab,
  () => {
    const next = tabFromQuery()
    activeTab.value = next
    const raw = Array.isArray(route.query.tab) ? route.query.tab[0] : route.query.tab
    if (raw === 'gnss') navigateTo({ path: route.path, query: { ...route.query, tab: 'gps' } }, { replace: true })
  },
  { immediate: true },
)

watch(
  deviceId,
  () => {
    deviceConfig.value = null
    gnssRevision.value = 0
    loadDeviceConfig()
    refreshState()
  },
  { immediate: true },
)
</script>
