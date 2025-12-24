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
        <div class="readings-grid large">
          <div class="reading-card primary">
            <div class="reading-label">U1</div>
            <div class="reading-value big">{{ device?.state?.voltage1?.toFixed?.(6) ?? '—' }}</div>
            <div class="reading-sub">В</div>
          </div>
          <div class="reading-card primary">
            <div class="reading-label">U2</div>
            <div class="reading-value big">{{ device?.state?.voltage2?.toFixed?.(6) ?? '—' }}</div>
            <div class="reading-sub">В</div>
          </div>
        </div>
      </div>
      <div class="temps">
        <div v-for="(t, i) in temps" :key="i" class="temp-card">
          <div class="temp-title">{{ tempLabels[i] || `T${i + 1}` }}</div>
          <div class="temp-value small">{{ t?.toFixed?.(2) ?? '--' }} °C</div>
        </div>
        <div class="temp-card">
          <div class="temp-title">I</div>
          <div class="temp-value small">{{ device?.state?.inaCurrent?.toFixed?.(3) ?? '—' }} A</div>
        </div>
        <div class="temp-card">
          <div class="temp-title">P</div>
          <div class="temp-value small">{{ device?.state?.inaPower?.toFixed?.(3) ?? '—' }} W</div>
        </div>
        <div class="temp-card warm">
          <div class="temp-title">Нагрев</div>
          <div class="temp-value small">{{ device?.state?.heaterPower?.toFixed?.(1) ?? '—' }} %</div>
        </div>
        <div class="temp-card cool">
          <div class="temp-title">Вентилятор</div>
          <div class="temp-value small">{{ device?.state?.fanPower?.toFixed?.(1) ?? '—' }} %</div>
        </div>
        <div class="temp-card subtle">
          <div class="temp-title">FAN1</div>
          <div class="temp-value small">{{ device?.state?.fan1Rpm ?? '—' }} rpm</div>
        </div>
        <div class="temp-card subtle">
          <div class="temp-title">FAN2</div>
          <div class="temp-value small">{{ device?.state?.fan2Rpm ?? '—' }} rpm</div>
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
        <div class="inline fields">
          <label class="checkbox"><input type="checkbox" v-model="log.useMotor" /> <span>Использовать мотор</span></label>
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
        <label class="inline checkbox"><input type="checkbox" v-model="stepper.reverse" /> <span>Реверс</span></label>
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
const tempLabels = computed(() => device.value?.state?.tempLabels ?? [])

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
.card { display: flex; flex-direction: column; gap: 10px; resize: vertical; overflow-y: auto; overflow-x: hidden; max-width: 100%; box-sizing: border-box; }
.card-head { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
.metrics { border: 1px solid var(--border); }
.metrics-top { display: flex; flex-direction: column; gap: 14px; }
.readings-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: 10px; }
.readings-grid.large { grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); }
.reading-card { background: #f8fafc; border: 1px solid var(--border); border-radius: 12px; padding: 12px; min-height: 96px; display: flex; flex-direction: column; gap: 4px; box-shadow: 0 2px 8px rgba(0,0,0,0.04); font-variant-numeric: tabular-nums; }
.reading-card.primary { background: linear-gradient(180deg, #e8f4fd, #f8fbff); }
.reading-card.warm { background: #fff7ec; }
.reading-card.cool { background: #eef6ff; }
.reading-card.subtle { background: #fafafa; }
.reading-label { font-size: 13px; color: #4a5568; font-weight: 700; }
.reading-value { font-size: 20px; font-weight: 800; color: #1f2d3d; }
.reading-value.big { font-size: 26px; }
.reading-sub { font-size: 12px; color: var(--muted); }
.chip { padding: 6px 10px; border-radius: 999px; background: #f1f3f4; border: 1px solid var(--border); color: var(--muted); font-weight: 600; word-break: break-word; white-space: normal; }
.chip.strong { background: #e8f4fd; border-color: rgba(41, 128, 185, 0.3); color: #1f2d3d; }
.chip.subtle { background: #fafafa; }
.chip.warm { background: #fff3e8; color: #b54708; border-color: rgba(229, 152, 102, 0.4); }
.chip.cool { background: #e7f5ff; color: #22649e; border-color: rgba(52, 152, 219, 0.35); }
.chip.online { background: #e6f8ed; color: #2e8b57; border-color: rgba(46, 139, 87, 0.35); }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
.inline { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.inline > * { min-width: 0; }
.fields > * { flex: 1 1 180px; }
.fields .compact input { width: 100%; }
.checkbox { display: inline-flex; align-items: center; justify-content: flex-start; gap: 10px; font-weight: 600; white-space: nowrap; }
.checkbox input { margin: 0; width: 18px; height: 18px; accent-color: var(--accent); flex-shrink: 0; }
.compact input { width: 120px; }
.form-group { display: flex; flex-direction: column; gap: 6px; }
label { font-size: 14px; font-weight: 600; color: #1f1f1f; }
input { width: 100%; box-sizing: border-box; }
.actions { display: flex; gap: 10px; flex-wrap: wrap; }
.muted { color: var(--muted); font-size: 13px; }
.temps { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; margin-top: 6px; }
.temp-card { background: linear-gradient(180deg, #f9fbff, #eef3fb); border: 1px solid var(--border); border-radius: 12px; padding: 10px; box-shadow: 0 2px 6px rgba(52, 152, 219, 0.08); font-variant-numeric: tabular-nums; min-height: 78px; display: flex; flex-direction: column; gap: 4px; }
.temp-title { font-size: 12px; color: var(--muted); }
.temp-value { font-weight: 700; font-size: 16px; color: #1f2d3d; }
.temp-value.small { font-size: 16px; }
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
