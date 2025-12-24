<template>
  <div class="page" v-if="deviceId">
    <button class="back" @click="navigateTo('/')">← Назад</button>
    <h1>Устройство {{ deviceId }}</h1>
    <div class="status-row">
      <span :class="{ online: device?.online }">{{ device?.online ? 'Online' : 'Offline' }}</span>
      <button @click="refreshState">Обновить состояние</button>
    </div>

    <div class="grid">
      <div class="card">
        <h3>Логи</h3>
        <label>Имя файла <input v-model="log.filename" /></label>
        <label><input type="checkbox" v-model="log.useMotor" /> Использовать мотор</label>
        <label>Длительность (с) <input type="number" v-model.number="log.durationSec" min="0.1" step="0.1" /></label>
        <div class="actions">
          <button @click="startLog">Старт</button>
          <button @click="stopLog">Стоп</button>
        </div>
        <p>Текущий файл: {{ device?.state?.logFilename || '—' }}</p>
      </div>

      <div class="card">
        <h3>Шаговик</h3>
        <div class="actions">
          <button @click="stepperEnable">Enable</button>
          <button @click="stepperDisable">Disable</button>
        </div>
        <label>Шаги <input type="number" v-model.number="stepper.steps" /></label>
        <label>Скорость, us <input type="number" v-model.number="stepper.speedUs" /></label>
        <label><input type="checkbox" v-model="stepper.reverse" /> Реверс</label>
        <button @click="stepperMove">Движение</button>
        <p>Pos: {{ device?.state?.stepperPosition }} Target: {{ device?.state?.stepperTarget }}</p>
      </div>

      <div class="card">
        <h3>Нагреватель / Вентилятор</h3>
        <label>Мощность нагревателя (%) <input type="number" v-model.number="heaterPower" min="0" max="100" /></label>
        <button @click="setHeater">Установить</button>
        <label>Мощность вентилятора (%) <input type="number" v-model.number="fanPower" min="0" max="100" /></label>
        <button @click="setFan">Установить</button>
      </div>

      <div class="card">
        <h3>Показания</h3>
        <p>U1: {{ device?.state?.voltage1?.toFixed?.(6) }}</p>
        <p>U2: {{ device?.state?.voltage2?.toFixed?.(6) }}</p>
        <p>I: {{ device?.state?.inaCurrent?.toFixed?.(3) }}</p>
        <p>P: {{ device?.state?.inaPower?.toFixed?.(3) }}</p>
        <p>Температуры: <span v-for="(t, i) in temps" :key="i">{{ t?.toFixed?.(2) }}°C </span></p>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useDevicesStore } from '~/stores/devices'

const route = useRoute()
const deviceId = computed(() => route.params.deviceId as string)
const store = useDevicesStore()
const nuxtApp = useNuxtApp()
store.init(nuxtApp.$mqtt)

const device = computed(() => (deviceId.value ? store.devices.get(deviceId.value) : undefined))

const log = reactive({ filename: 'data', useMotor: false, durationSec: 1 })
const stepper = reactive({ steps: 400, speedUs: 1000, reverse: false })
const heaterPower = ref(0)
const fanPower = ref(0)

async function refreshState() {
  if (!deviceId.value) return
  try {
    await store.getState(nuxtApp.$mqtt, deviceId.value)
  } catch (e) {
    console.error(e)
  }
}

function startLog() {
  if (!deviceId.value) return
  store.logStart(nuxtApp.$mqtt, deviceId.value, { ...log })
}
function stopLog() {
  if (!deviceId.value) return
  store.logStop(nuxtApp.$mqtt, deviceId.value)
}
function stepperEnable() {
  if (!deviceId.value) return
  store.stepperEnable(nuxtApp.$mqtt, deviceId.value)
}
function stepperDisable() {
  if (!deviceId.value) return
  store.stepperDisable(nuxtApp.$mqtt, deviceId.value)
}
function stepperMove() {
  if (!deviceId.value) return
  store.stepperMove(nuxtApp.$mqtt, deviceId.value, { ...stepper })
}
function setHeater() {
  if (!deviceId.value) return
  store.heaterSet(nuxtApp.$mqtt, deviceId.value, heaterPower.value)
}
function setFan() {
  if (!deviceId.value) return
  store.fanSet(nuxtApp.$mqtt, deviceId.value, fanPower.value)
}

onMounted(() => {
  refreshState()
})
</script>

<style scoped>
.page { max-width: 960px; margin: 0 auto; padding: 24px; display: flex; flex-direction: column; gap: 16px; }
.back { align-self: flex-start; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 16px; }
.card { border: 1px solid #ddd; border-radius: 8px; padding: 12px; display: flex; flex-direction: column; gap: 8px; }
.actions { display: flex; gap: 8px; flex-wrap: wrap; }
.status-row { display: flex; gap: 12px; align-items: center; }
.online { color: #2ecc71; }
label { display: flex; flex-direction: column; gap: 4px; font-size: 14px; }
input { padding: 6px; border-radius: 4px; border: 1px solid #ccc; }
button { padding: 6px 10px; border: 1px solid #888; border-radius: 6px; cursor: pointer; background: #f7f7f7; }
button:hover { background: #eee; }
</style>
