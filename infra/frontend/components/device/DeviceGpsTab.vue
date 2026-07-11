<template>
  <div class="device-feature-stack">
    <DeviceGpsPanel
      :has-gps="hasGps"
      :actual-mode="actualMode"
      :antenna-short="antennaShort"
      :antenna-short-raw="antennaShortRaw"
      :position-valid="positionValid"
      :latitude="numberState('gpsLat')"
      :longitude="numberState('gpsLon')"
      :altitude="numberState('gpsAlt')"
      :fix-quality="numberState('gpsFixQuality')"
      :satellites="numberState('gpsSatellites')"
      :position-age-ms="numberState('gpsPositionAgeMs') ?? numberState('gpsFixAgeMs')"
      :time-valid="state.gpsTimeValid === true"
      :time-iso="String(state.gpsTimeIso || '')"
      :time-age-ms="numberState('gpsTimeAgeMs')"
      :form="form"
      :saving="saving"
      :status="status"
      @save="save"
      @probe="probe"
      @update:mode="updateMode"
      @update:rtcm-text="updateRtcmText"
    />
    <GnssDataManager :device-id="deviceId" @changed="$emit('gnss-changed')" />
  </div>
</template>

<script setup lang="ts">
import GnssDataManager from './gps/GnssDataManager.vue'
import type { DeviceGpsConfig } from '~/types/device'
import { useDevicesStore } from '~/stores/devices'

const props = defineProps<{ deviceId: string; state: Record<string, any> }>()
defineEmits<{ 'gnss-changed': [] }>()

const { apiFetch } = useApi()
const store = useDevicesStore()
const nuxtApp = useNuxtApp()
store.init(nuxtApp.$mqtt)

const config = ref<DeviceGpsConfig | null>(null)
const dirty = ref(false)
const saving = ref(false)
const status = ref('')
const form = reactive({ mode: 'base_time_60', rtcmText: '1004,1006,1033' })

const numberState = (key: string) => {
  const value = Number(props.state?.[key])
  return Number.isFinite(value) ? value : null
}
const liveTypes = computed(() => Array.isArray(props.state?.gpsRtcmTypes)
  ? props.state.gpsRtcmTypes.map(Number).filter(Number.isFinite)
  : [])
const actualMode = computed(() => String(props.state?.gpsActualMode || config.value?.actual_mode || ''))
const antennaShort = computed(() => typeof props.state?.gpsAntennaShort === 'boolean' ? props.state.gpsAntennaShort : null)
const antennaShortRaw = computed(() => numberState('gpsAntennaShortRaw'))
const positionValid = computed(() => props.state?.gpsPositionValid === true)
const hasGps = computed(() => !!config.value?.has_gps || liveTypes.value.length > 0 || !!props.state?.gpsMode || !!actualMode.value || positionValid.value || props.state?.gpsTimeValid === true)

const seed = () => {
  const types = liveTypes.value.length ? liveTypes.value : config.value?.rtcm_types?.length ? config.value.rtcm_types : [1004, 1006, 1033]
  form.rtcmText = types.join(',')
  form.mode = String(props.state?.gpsMode || config.value?.mode || 'base_time_60')
  dirty.value = false
}

const parseTypes = () => [...new Set(form.rtcmText.split(/[,\s;]+/).map(Number)
  .filter((value) => Number.isInteger(value) && value > 0 && value <= 4095))].sort((a, b) => a - b)

const load = async () => {
  if (!props.deviceId) return
  try {
    config.value = await apiFetch<DeviceGpsConfig>(`/api/devices/${props.deviceId}/gps`)
    status.value = ''
    if (!dirty.value) seed()
  } catch (error: any) {
    status.value = error?.message || 'Не удалось загрузить GPS config'
  }
}

const save = async () => {
  const rtcmTypes = parseTypes()
  if (!rtcmTypes.length) { status.value = 'Укажи хотя бы один RTCM код'; return }
  saving.value = true
  status.value = 'Сохраняю GPS config...'
  try {
    config.value = await apiFetch<DeviceGpsConfig>(`/api/devices/${props.deviceId}/gps`, {
      method: 'PATCH', body: { has_gps: true, rtcm_types: rtcmTypes, mode: form.mode },
    })
    await store.gpsApply(nuxtApp.$mqtt, props.deviceId, { mode: form.mode, rtcmTypes })
    dirty.value = false
    status.value = 'GPS config сохранен, команда отправлена'
  } catch (error: any) {
    status.value = error?.message || 'Не удалось применить GPS config'
  } finally { saving.value = false }
}

const probe = async () => {
  saving.value = true
  status.value = 'Запрашиваю MODE...'
  try {
    await store.gpsProbe(nuxtApp.$mqtt, props.deviceId)
    status.value = 'Команда MODE отправлена'
  } catch (error: any) {
    status.value = error?.message || 'Не удалось запросить MODE'
  } finally { saving.value = false }
}

const updateMode = (value: string) => { form.mode = value; dirty.value = true }
const updateRtcmText = (value: string) => { form.rtcmText = value; dirty.value = true }

watch(() => props.deviceId, () => { config.value = null; status.value = ''; dirty.value = false; load() })
watch(() => props.state, () => { if (!dirty.value) seed() }, { deep: true })
onMounted(load)
</script>
