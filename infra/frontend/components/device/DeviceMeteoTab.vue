<template>
  <div class="device-feature-stack">
    <div class="card">
      <div class="card-head">
        <h3>Метеостанция WN90LP</h3>
        <span v-if="!hasMeteo" class="badge subtle">не обнаружена</span>
        <span v-else-if="state.meteoOnline" class="badge success">онлайн</span>
        <span v-else class="badge danger">офлайн</span>
      </div>
      <div v-if="!hasMeteo" class="muted" style="padding:1rem">
        Метеостанция не зарегистрирована. История появится после первого успешного опроса WN90LP.
      </div>
      <table v-else class="data-table" style="margin-top:.5rem">
        <tbody>
          <tr><td class="label-cell">Температура</td><td>{{ live('meteoTemp', 1, ' °C') }}</td></tr>
          <tr><td class="label-cell">Влажность</td><td>{{ live('meteoHumidity', 0, ' %') }}</td></tr>
          <tr><td class="label-cell">Давление</td><td>{{ live('meteoPressure', 1, ' hPa') }}</td></tr>
          <tr><td class="label-cell">Ветер</td><td>{{ windLabel }}</td></tr>
          <tr><td class="label-cell">Порывы</td><td>{{ live('meteoGustSpeed', 1, ' м/с') }}</td></tr>
          <tr><td class="label-cell">Осадки</td><td>{{ live('meteoRainfall', 1, ' мм') }}</td></tr>
          <tr><td class="label-cell">Освещённость</td><td>{{ live('meteoLight', 0, ' лк') }}</td></tr>
          <tr><td class="label-cell">УФ-индекс</td><td>{{ live('meteoUvi', 1, '') }}</td></tr>
          <tr v-if="state.meteoTimestampMs"><td class="label-cell">Последний опрос</td><td class="muted">{{ new Date(state.meteoTimestampMs).toLocaleString() }}</td></tr>
        </tbody>
      </table>
      <div class="divider"></div>
      <h4>Интервалы WN90LP</h4>
      <div class="meteo-config-grid">
        <div class="form-group">
          <label for="meteo-poll-interval">Обновление состояния, сек</label>
          <input id="meteo-poll-interval" v-model.number="form.pollIntervalS" type="number" min="1" max="3600" step="1" @input="dirty = true" />
          <span class="muted">Опрос станции и обновление live-данных: 1–3600 сек.</span>
        </div>
        <div class="form-group">
          <label for="meteo-file-interval">Запись в CSV, сек</label>
          <input id="meteo-file-interval" v-model.number="form.fileIntervalS" type="number" min="10" max="86400" step="1" @input="dirty = true" />
          <span class="muted">Запись последнего валидного измерения: 10–86400 сек.</span>
        </div>
      </div>
      <button class="btn primary" :disabled="saving" @click="saveConfig">
        {{ saving ? 'Сохраняю…' : 'Сохранить интервалы' }}
      </button>
      <p v-if="status" class="muted">{{ status }}</p>
    </div>
    <MeteoHistoryPanel :device-id="deviceId" />
  </div>
</template>

<script setup lang="ts">
import MeteoHistoryPanel from './meteo/MeteoHistoryPanel.vue'
import { useDevicesStore } from '~/stores/devices'

const props = defineProps<{ deviceId: string; state: Record<string, any>; configured: boolean }>()
const store = useDevicesStore()
const nuxtApp = useNuxtApp()
store.init(nuxtApp.$mqtt)
const form = reactive({ pollIntervalS: 9, fileIntervalS: 60 })
const dirty = ref(false)
const saving = ref(false)
const status = ref('')
const hasMeteo = computed(() => props.configured || props.state?.meteoOnline === true || Number.isFinite(Number(props.state?.meteoTimestampMs)))
const live = (key: string, digits: number, suffix: string) => {
  if (!props.state?.meteoOnline) return '—'
  const value = Number(props.state?.[key])
  return Number.isFinite(value) ? `${value.toFixed(digits)}${suffix}` : '—'
}
const windLabel = computed(() => {
  if (!props.state?.meteoOnline) return '—'
  const speed = Number(props.state?.meteoWindSpeed)
  const direction = Number(props.state?.meteoWindDir)
  const speedText = Number.isFinite(speed) ? `${speed.toFixed(1)} м/с` : '—'
  return Number.isFinite(direction) ? `${speedText}, ${direction.toFixed(0)}°` : speedText
})

const seedConfig = () => {
  const poll = Number(props.state?.meteoPollIntervalS)
  const file = Number(props.state?.meteoFileIntervalS)
  form.pollIntervalS = Number.isInteger(poll) ? poll : 9
  form.fileIntervalS = Number.isInteger(file) ? file : 60
  dirty.value = false
}

const saveConfig = async () => {
  if (!Number.isInteger(form.pollIntervalS) || form.pollIntervalS < 1 || form.pollIntervalS > 3600) {
    status.value = 'Интервал состояния должен быть целым числом от 1 до 3600 секунд'
    return
  }
  if (!Number.isInteger(form.fileIntervalS) || form.fileIntervalS < 10 || form.fileIntervalS > 86400) {
    status.value = 'Интервал CSV должен быть целым числом от 10 до 86400 секунд'
    return
  }
  saving.value = true
  status.value = 'Сохраняю config.txt на SD и резервную копию в NVS…'
  try {
    await store.meteoConfigApply(nuxtApp.$mqtt, props.deviceId, {
      pollIntervalS: form.pollIntervalS,
      fileIntervalS: form.fileIntervalS,
    })
    dirty.value = false
    status.value = 'Интервалы сохранены в SD и NVS и уже применены'
  } catch (error: any) {
    status.value = error?.message || 'Не удалось сохранить интервалы'
  } finally {
    saving.value = false
  }
}

watch(() => props.state, () => { if (!dirty.value && !saving.value) seedConfig() }, { deep: true })
watch(() => props.deviceId, () => { status.value = ''; seedConfig() })
onMounted(seedConfig)
</script>

<style scoped>
.meteo-config-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
  gap: 1rem;
}
</style>
