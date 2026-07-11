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
    </div>
    <MeteoHistoryPanel :device-id="deviceId" />
  </div>
</template>

<script setup lang="ts">
import MeteoHistoryPanel from './meteo/MeteoHistoryPanel.vue'

const props = defineProps<{ deviceId: string; state: Record<string, any>; configured: boolean }>()
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
</script>
