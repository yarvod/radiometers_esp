<template>
  <div class="page" v-if="deviceId">
    <div class="header">
      <button class="btn ghost back" @click="navigateTo('/')">← Назад</button>
      <div>
        <div class="title">Устройство {{ deviceId }}</div>
        <div class="status-row">
          <span class="chip" :class="{ online: device?.online }">{{ device?.online ? 'Online' : 'Offline' }}</span>
          <span class="chip subtle" v-if="device?.lastSeen">Обновлено: {{ new Date(device.lastSeen).toLocaleTimeString() }}</span>
          <button class="btn primary sm" @click="refreshState">Обновить состояние</button>
        </div>
      </div>
    </div>

    <div class="card metrics">
      <div class="metrics-top">
        <h3>Показания</h3>
        <div class="chips">
          <span class="chip strong">U1: {{ device?.state?.voltage1?.toFixed?.(6) ?? '—' }}</span>
          <span class="chip strong">U2: {{ device?.state?.voltage2?.toFixed?.(6) ?? '—' }}</span>
          <span class="chip strong">I: {{ device?.state?.inaCurrent?.toFixed?.(3) ?? '—' }}</span>
          <span class="chip strong">P: {{ device?.state?.inaPower?.toFixed?.(3) ?? '—' }}</span>
        </div>
      </div>
      <div class="temps">
        <div v-for="(t, i) in temps" :key="i" class="temp-card">
          <div class="temp-title">T{{ i + 1 }}</div>
          <div class="temp-value">{{ t?.toFixed?.(2) ?? '--' }} °C</div>
        </div>
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="card-head">
          <h3>Логи</h3>
          <span class="badge">Файлы</span>
        </div>
        <div class="form-group">
          <label>Имя файла</label>
          <input v-model="log.filename" />
        </div>
        <div class="inline">
          <label class="checkbox"><input type="checkbox" v-model="log.useMotor" /> Использовать мотор</label>
          <label class="compact">Длительность (с)
            <input type="number" v-model.number="log.durationSec" min="0.1" step="0.1" />
          </label>
        </div>
        <div class="actions">
          <button class="btn primary" @click="startLog">Старт</button>
          <button class="btn warning ghost" @click="stopLog">Стоп</button>
        </div>
        <p class="muted">Текущий файл: {{ device?.state?.logFilename || '—' }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Шаговик</h3>
          <span class="badge success">Мотор</span>
        </div>
        <div class="inline">
          <button class="btn success" @click="stepperEnable">Enable</button>
          <button class="btn ghost" @click="stepperDisable">Disable</button>
        </div>
        <div class="form-group">
          <label>Шаги</label>
          <input type="number" v-model.number="stepper.steps" />
        </div>
        <div class="form-group">
          <label>Скорость, us</label>
          <input type="number" v-model.number="stepper.speedUs" />
        </div>
        <label class="inline checkbox"><input type="checkbox" v-model="stepper.reverse" /> Реверс</label>
        <button class="btn primary" @click="stepperMove">Движение</button>
        <p class="muted">Pos: {{ device?.state?.stepperPosition }} Target: {{ device?.state?.stepperTarget }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Нагреватель / Вентилятор</h3>
          <span class="badge accent">Термоконтроль</span>
        </div>
        <div class="form-group">
          <label>Мощность нагревателя (%)</label>
          <input type="number" v-model.number="heaterPower" min="0" max="100" />
        </div>
        <button class="btn danger" @click="setHeater">Установить</button>
        <div class="form-group">
          <label>Мощность вентилятора (%)</label>
          <input type="number" v-model.number="fanPower" min="0" max="100" />
        </div>
        <button class="btn success" @click="setFan">Установить</button>
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
const temps = computed(() => device.value?.state?.temps ?? [])

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
.page { max-width: 1100px; margin: 0 auto; padding: 24px; display: flex; flex-direction: column; gap: 18px; }
.header { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
.title { font-size: 24px; font-weight: 800; color: #1f2933; }
.status-row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.card { display: flex; flex-direction: column; gap: 10px; resize: vertical; overflow: auto; }
.card-head { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
.metrics { border: 1px solid var(--border); }
.metrics-top { display: flex; flex-direction: column; gap: 10px; }
.chips { display: flex; flex-wrap: wrap; gap: 8px; }
.chip { padding: 6px 10px; border-radius: 999px; background: #f1f3f4; border: 1px solid var(--border); color: var(--muted); font-weight: 600; }
.chip.strong { background: #e8f4fd; border-color: rgba(41, 128, 185, 0.3); color: #1f2d3d; }
.chip.subtle { background: #fafafa; }
.chip.online { background: #e6f8ed; color: #2e8b57; border-color: rgba(46, 139, 87, 0.35); }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
.inline { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.checkbox { gap: 6px; font-weight: 600; }
.compact input { width: 120px; }
.form-group { display: flex; flex-direction: column; gap: 6px; }
label { font-size: 14px; font-weight: 600; color: #1f1f1f; }
input { width: 100%; }
.actions { display: flex; gap: 10px; flex-wrap: wrap; }
.muted { color: var(--muted); font-size: 13px; }
.temps { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px; margin-top: 6px; }
.temp-card { background: linear-gradient(180deg, #f9fbff, #eef3fb); border: 1px solid var(--border); border-radius: 12px; padding: 12px; box-shadow: 0 2px 8px rgba(52, 152, 219, 0.12); }
.temp-title { font-size: 13px; color: var(--muted); }
.temp-value { font-weight: 700; font-size: 20px; color: #1f2d3d; }
.badge { padding: 4px 10px; border-radius: 999px; background: #ecf0f1; font-weight: 700; font-size: 12px; color: #4a5568; }
.badge.success { background: #e6f8ed; color: #2e8b57; }
.badge.accent { background: #e8f4fd; color: #1f2d3d; }
.btn { border-radius: 12px; }
.btn.sm { padding: 8px 12px; font-size: 14px; }
.btn.primary { background: linear-gradient(135deg, #3498db, #2980b9); }
.btn.success { background: linear-gradient(135deg, #27ae60, #219a52); }
.btn.warning { background: linear-gradient(135deg, #e67e22, #d35400); }
.btn.danger { background: linear-gradient(135deg, #e74c3c, #c0392b); }
.btn.ghost { background: #f5f7fb; color: #1f1f1f; border: 1px solid var(--border); box-shadow: none; }
.back { align-self: flex-start; }
</style>
