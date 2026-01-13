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
          <div class="reading-card primary">
            <div class="reading-label">U3</div>
            <div class="reading-value big">{{ device?.state?.voltage3?.toFixed?.(6) ?? '—' }}</div>
            <div class="reading-sub">В</div>
          </div>
        </div>
      </div>
      <div class="temps">
        <div class="temp-card subtle">
          <div class="temp-title">Wi‑Fi RSSI</div>
          <div class="temp-value small">{{ device?.state?.wifiRssi ?? '--' }} dBm</div>
        </div>
        <div class="temp-card subtle">
          <div class="temp-title">Wi‑Fi качество</div>
          <div class="temp-value small">{{ device?.state?.wifiQuality ?? '--' }}%</div>
        </div>
        <div v-for="sensor in tempEntries" :key="sensor.key" class="temp-card">
          <div class="temp-title">{{ sensor.label }}</div>
          <div class="temp-value small">{{ sensor.value?.toFixed?.(2) ?? '--' }} °C</div>
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
          <h3>Термоконтроль</h3>
          <span class="badge accent">Heater + PID</span>
        </div>
        <div class="status-row">
          <span class="chip" :class="{ online: pidEnabled, subtle: !pidEnabled }">PID {{ pidEnabled ? 'On' : 'Off' }}</span>
          <span class="chip subtle">Выход: {{ pidOutputDisplay }}</span>
          <span class="chip subtle">Цель: {{ pidSetpointDisplay }}</span>
          <span class="chip subtle">Датчик: {{ pidSensorLabel }}</span>
          <span class="chip subtle">T: {{ pidSensorTemp }}</span>
        </div>
        <div class="form-group">
          <label>Ручной нагрев (%)</label>
          <div class="range-row">
            <input type="range" min="0" max="100" step="0.5" v-model.number="heaterPower" @input="heaterEditing = true" @change="heaterEditing = true" />
            <input type="number" min="0" max="100" step="0.1" v-model.number="heaterPower" @input="heaterEditing = true" />
          </div>
        </div>
        <button class="btn danger" @click="setHeater">Установить</button>
        <div class="divider"></div>
        <div class="form-group">
          <label>PID цель (°C)</label>
          <input type="number" min="0" step="0.1" v-model.number="pidForm.setpoint" @input="pidDirty = true" />
        </div>
        <div class="form-group">
          <label>PID датчики</label>
          <div class="chip-select">
            <button
              v-for="(sensor, idx) in tempEntries"
              :key="sensor.key"
              class="chip-option"
              :class="{ selected: pidForm.sensorIndices.includes(idx) }"
              type="button"
              @click="togglePidSensor(idx)"
            >
              {{ sensor.label }}{{ sensor.address ? ` (${sensor.address})` : '' }}
            </button>
            <span v-if="tempEntries.length === 0" class="muted">Нет датчиков</span>
          </div>
        </div>
        <div class="inline fields">
          <label class="compact">Kp
            <input type="number" step="0.01" v-model.number="pidForm.kp" @input="pidDirty = true" />
          </label>
          <label class="compact">Ki
            <input type="number" step="0.01" v-model.number="pidForm.ki" @input="pidDirty = true" />
          </label>
          <label class="compact">Kd
            <input type="number" step="0.01" v-model.number="pidForm.kd" @input="pidDirty = true" />
          </label>
        </div>
        <div class="actions">
          <button class="btn primary" @click="applyPid">Сохранить PID</button>
          <button class="btn success" @click="enablePid">Включить PID</button>
          <button class="btn warning ghost" @click="disablePid">Выключить PID</button>
        </div>
        <p class="muted" v-if="pidApplyStatus">{{ pidApplyStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head">
          <h3>Шаговик</h3>
          <span class="badge success">Мотор</span>
        </div>
        <div class="status-row">
          <span class="chip" :class="{ online: stepperEnabled, subtle: !stepperEnabled }">{{ stepperEnabled ? 'Enabled' : 'Disabled' }}</span>
          <span class="chip" :class="{ warm: stepperMoving, subtle: !stepperMoving }">{{ stepperMoving ? 'Moving' : 'Idle' }}</span>
          <span class="chip" :class="{ warn: stepperHoming, subtle: !stepperHoming }">{{ stepperHoming ? 'Homing' : 'Ready' }}</span>
          <span class="chip subtle">Dir: {{ stepperDirText }}</span>
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
          <h3>Wi‑Fi</h3>
          <span class="badge">Сеть</span>
        </div>
        <div class="status-row">
          <span class="chip strong">IP: {{ wifiIpDisplay }}</span>
          <span class="chip subtle">STA: {{ wifiStaIpDisplay }}</span>
          <span class="chip subtle">AP: {{ wifiApIpDisplay }}</span>
          <span class="chip" :class="{ online: wifiModeDisplay === 'sta', cool: wifiModeDisplay === 'ap' }">Mode: {{ wifiModeDisplay.toUpperCase() }}</span>
        </div>
        <div class="status-row">
          <span class="chip subtle">RSSI: {{ device?.state?.wifiRssi ?? '--' }} dBm</span>
          <span class="chip subtle">Качество: {{ device?.state?.wifiQuality ?? '--' }}%</span>
          <span class="chip subtle">SSID: {{ wifiSsidDisplay }}</span>
        </div>
        <div class="form-group">
          <label>Режим</label>
          <select v-model="wifiForm.mode" @change="wifiDirty = true">
            <option value="sta">STA (подключение)</option>
            <option value="ap">AP (точка доступа)</option>
          </select>
        </div>
        <div class="form-group">
          <label>SSID</label>
          <input type="text" v-model="wifiForm.ssid" @input="wifiDirty = true" />
        </div>
        <div class="form-group">
          <label>Пароль</label>
          <input type="password" v-model="wifiForm.password" @input="wifiDirty = true" />
        </div>
        <button class="btn primary" @click="applyWifi">Применить Wi‑Fi</button>
        <p class="muted" v-if="wifiApplyStatus">{{ wifiApplyStatus }}</p>
      </div>
    </div>

    <div class="card">
      <div class="card-head">
        <h3>История измерений</h3>
        <span class="badge">Графики</span>
      </div>
      <div class="inline fields">
        <label class="compact">C
          <input type="datetime-local" v-model="historyFilters.from" />
        </label>
        <label class="compact">По
          <input type="datetime-local" v-model="historyFilters.to" />
        </label>
        <label class="compact">Лимит
          <input type="number" min="100" max="10000" step="100" v-model.number="historyFilters.limit" />
        </label>
      </div>
      <div class="actions">
        <button class="btn primary" @click="loadHistory" :disabled="historyLoading">Загрузить</button>
        <span class="muted" v-if="historyStatus">{{ historyStatus }}</span>
      </div>
      <div class="form-group">
        <label>Температурные датчики</label>
        <div class="chip-select">
          <button
            v-for="(sensor, idx) in tempEntries"
            :key="`history-temp-${sensor.key}`"
            type="button"
            class="chip-option"
            :class="{ selected: historySelection.tempIndices.includes(idx) }"
            @click="toggleHistoryTemp(idx)"
          >
            {{ sensor.label }}
          </button>
          <span v-if="tempEntries.length === 0" class="muted">Нет датчиков</span>
        </div>
      </div>
      <div class="form-group">
        <label>Серии ADC</label>
        <div class="chip-select">
          <button type="button" class="chip-option" :class="{ selected: historySelection.showAdc }" @click="historySelection.showAdc = !historySelection.showAdc">
            ADC
          </button>
          <button type="button" class="chip-option" :class="{ selected: historySelection.showCal }" @click="historySelection.showCal = !historySelection.showCal">
            Cal
          </button>
        </div>
      </div>
      <div class="chart-stack">
        <div class="chart-box">
          <h4>Температуры</h4>
          <ClientOnly>
            <Plotly :data="tempPlotData" :layout="tempPlotLayout" :config="plotConfig" />
          </ClientOnly>
        </div>
        <div class="chart-box">
          <h4>ADC + Cal</h4>
          <ClientOnly>
            <Plotly :data="adcPlotData" :layout="adcPlotLayout" :config="plotConfig" />
          </ClientOnly>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useDevicesStore } from '~/stores/devices'

type MeasurementRow = {
  id: string
  device_id: string
  timestamp: string
  timestamp_ms: number | null
  adc1: number
  adc2: number
  adc3: number
  temps: number[]
  bus_v: number
  bus_i: number
  bus_p: number
  adc1_cal: number | null
  adc2_cal: number | null
  adc3_cal: number | null
  log_use_motor: boolean
  log_duration: number
  log_filename: string | null
}

const { apiFetch } = useApi()
const route = useRoute()
const deviceId = computed(() => route.params.deviceId as string)
const store = useDevicesStore()
const nuxtApp = useNuxtApp()
store.init(nuxtApp.$mqtt)

const device = computed(() => (deviceId.value ? store.devices.get(deviceId.value) : undefined))
const tempEntries = computed(() => {
  const sensors = device.value?.state?.tempSensors
  if (Array.isArray(sensors)) {
    return sensors.map((value, idx) => ({ key: `t${idx + 1}`, label: `t${idx + 1}`, value, address: '' }))
  }
  if (!sensors || typeof sensors !== 'object') return []
  return Object.entries(sensors as Record<string, any>).map(([label, entry]) => {
    if (entry && typeof entry === 'object') {
      return { key: label, label, value: entry.value, address: entry.address || '' }
    }
    return { key: label, label, value: entry, address: '' }
  })
})

const log = reactive({ filename: 'data', useMotor: false, durationSec: 1 })
const stepper = reactive({ steps: 400, speedUs: 1000, reverse: false })
const heaterPower = ref(0)
const heaterEditing = ref(false)
const pidDirty = ref(false)
const wifiDirty = ref(false)
const pidApplyStatus = ref('')
const wifiApplyStatus = ref('')
const pidForm = reactive({ setpoint: 25, sensorIndices: [] as number[], kp: 1, ki: 0, kd: 0 })
const wifiForm = reactive({ mode: 'sta', ssid: '', password: '' })
const historyFilters = reactive({
  from: '',
  to: '',
  limit: 2000,
})
const historySelection = reactive({
  tempIndices: [] as number[],
  showAdc: true,
  showCal: false,
})
const historyData = ref<MeasurementRow[]>([])
const historyLoading = ref(false)
const historyStatus = ref('')
const plotConfig = { displayModeBar: false, responsive: true }

const maskToIndices = (mask: number, count: number) => {
  const out: number[] = []
  for (let i = 0; i < count; i++) {
    if (mask & (1 << i)) out.push(i)
  }
  return out
}

const normalizeIndices = (indices: number[], count: number) => {
  const set = new Set(indices.filter((idx) => idx >= 0 && idx < count))
  return Array.from(set).sort((a, b) => a - b)
}

const pidStateSensorIndices = computed(() => {
  const count = tempEntries.value.length
  const state = device.value?.state || {}
  const mask = Number(state.pidSensorMask)
  if (Number.isFinite(mask) && mask > 0) {
    const indices = maskToIndices(mask, count)
    return indices.length ? indices : (count ? [0] : [])
  }
  const idx = Number(state.pidSensorIndex)
  if (Number.isFinite(idx) && count > 0) {
    return [Math.max(0, Math.min(count - 1, idx))]
  }
  return []
})

const pidEnabled = computed(() => !!device.value?.state?.pidEnabled)
const pidOutputDisplay = computed(() => {
  const val = device.value?.state?.pidOutput
  return Number.isFinite(val) ? `${Number(val).toFixed(1)} %` : '--'
})
const pidSetpointDisplay = computed(() => {
  const val = device.value?.state?.pidSetpoint
  return Number.isFinite(val) ? `${Number(val).toFixed(1)} °C` : '--'
})
const pidSensorLabel = computed(() => {
  if (pidStateSensorIndices.value.length === 0) return '--'
  return pidStateSensorIndices.value
    .map((idx) => tempEntries.value[idx]?.label || `T${idx + 1}`)
    .join(', ')
})
const pidSensorTemp = computed(() => {
  const indices = pidStateSensorIndices.value
  if (!indices.length) return '--'
  let sum = 0
  let count = 0
  indices.forEach((idx) => {
    const entry = tempEntries.value[idx]
    if (!entry || !Number.isFinite(entry.value)) return
    sum += Number(entry.value)
    count += 1
  })
  if (!count) return '--'
  return `${(sum / count).toFixed(2)} °C`
})

const stepperEnabled = computed(() => !!device.value?.state?.stepperEnabled)
const stepperMoving = computed(() => !!device.value?.state?.stepperMoving)
const stepperHoming = computed(() => !!device.value?.state?.stepperHoming)
const stepperDirText = computed(() => {
  const dir = device.value?.state?.stepperDirForward
  if (dir === undefined || dir === null) return '--'
  return dir ? 'FWD' : 'REV'
})

const wifiModeDisplay = computed(() => {
  const mode = device.value?.state?.wifiMode
  if (mode === 'ap' || mode === 'sta') return mode
  return device.value?.state?.wifiApMode ? 'ap' : 'sta'
})
const wifiIpDisplay = computed(() => device.value?.state?.wifiIp || '--')
const wifiStaIpDisplay = computed(() => device.value?.state?.wifiStaIp || '--')
const wifiApIpDisplay = computed(() => device.value?.state?.wifiApIp || '--')
const wifiSsidDisplay = computed(() => device.value?.state?.wifiSsid || '--')

const toLocalInputValue = (date: Date) => {
  const offset = date.getTimezoneOffset() * 60000
  return new Date(date.getTime() - offset).toISOString().slice(0, 16)
}

const palette = [
  '#1f77b4',
  '#ff7f0e',
  '#2ca02c',
  '#d62728',
  '#9467bd',
  '#8c564b',
  '#e377c2',
  '#7f7f7f',
]

const normalizeHistorySelection = (indices: number[], count: number) => {
  const normalized = normalizeIndices(indices, count)
  return normalized.length ? normalized : (count ? [0] : [])
}

const historyLabels = computed(() => historyData.value.map((row) => row.timestamp))

const tempPlotData = computed(() => {
  const selected = normalizeHistorySelection(historySelection.tempIndices, tempEntries.value.length)
  return selected.map((idx, seriesIdx) => {
    const color = palette[seriesIdx % palette.length]
    const data = historyData.value.map((row) => {
      const value = row.temps?.[idx]
      return Number.isFinite(value) ? Number(value) : null
    })
    const label = tempEntries.value[idx]?.label || `t${idx + 1}`
    return {
      x: historyLabels.value,
      y: data,
      type: 'scatter',
      mode: 'lines',
      name: label,
      line: { color, width: 2 },
    }
  })
})

const adcPlotData = computed(() => {
  const sets: any[] = []
  const baseColors = ['#1f77b4', '#2ca02c', '#d62728']
  const x = historyLabels.value
  if (historySelection.showAdc) {
    sets.push(
      { x, y: historyData.value.map((row) => row.adc1 ?? null), type: 'scatter', mode: 'lines', name: 'ADC1', line: { color: baseColors[0], width: 2 } },
      { x, y: historyData.value.map((row) => row.adc2 ?? null), type: 'scatter', mode: 'lines', name: 'ADC2', line: { color: baseColors[1], width: 2 } },
      { x, y: historyData.value.map((row) => row.adc3 ?? null), type: 'scatter', mode: 'lines', name: 'ADC3', line: { color: baseColors[2], width: 2 } },
    )
  }
  if (historySelection.showCal) {
    sets.push(
      { x, y: historyData.value.map((row) => row.adc1_cal ?? null), type: 'scatter', mode: 'lines', name: 'CAL1', line: { color: '#9ecae1', width: 2, dash: 'dot' } },
      { x, y: historyData.value.map((row) => row.adc2_cal ?? null), type: 'scatter', mode: 'lines', name: 'CAL2', line: { color: '#98df8a', width: 2, dash: 'dot' } },
      { x, y: historyData.value.map((row) => row.adc3_cal ?? null), type: 'scatter', mode: 'lines', name: 'CAL3', line: { color: '#ff9896', width: 2, dash: 'dot' } },
    )
  }
  return sets
})

const tempPlotLayout = computed(() => ({
  autosize: true,
  margin: { l: 40, r: 20, t: 10, b: 40 },
  paper_bgcolor: 'transparent',
  plot_bgcolor: 'transparent',
  xaxis: { title: 'Время', showgrid: false },
  yaxis: { title: '°C' },
  legend: { orientation: 'h', x: 0, y: -0.2 },
}))

const adcPlotLayout = computed(() => ({
  autosize: true,
  margin: { l: 40, r: 20, t: 10, b: 40 },
  paper_bgcolor: 'transparent',
  plot_bgcolor: 'transparent',
  xaxis: { title: 'Время', showgrid: false },
  yaxis: { title: 'V' },
  legend: { orientation: 'h', x: 0, y: -0.2 },
}))

watch(
  () => device.value?.state,
  (state) => {
    if (!state) return
    if (!pidDirty.value) {
      if (Number.isFinite(state.pidSetpoint)) pidForm.setpoint = Number(state.pidSetpoint)
      const mask = Number(state.pidSensorMask)
      if (Number.isFinite(mask) && mask > 0) {
        pidForm.sensorIndices = normalizeIndices(maskToIndices(mask, tempEntries.value.length), tempEntries.value.length)
      } else if (Number.isFinite(state.pidSensorIndex) && tempEntries.value.length > 0) {
        pidForm.sensorIndices = normalizeIndices([Number(state.pidSensorIndex)], tempEntries.value.length)
      }
      if (Number.isFinite(state.pidKp)) pidForm.kp = Number(state.pidKp)
      if (Number.isFinite(state.pidKi)) pidForm.ki = Number(state.pidKi)
      if (Number.isFinite(state.pidKd)) pidForm.kd = Number(state.pidKd)
    }
    if (!wifiDirty.value) {
      wifiForm.mode = state.wifiMode || (state.wifiApMode ? 'ap' : 'sta')
      wifiForm.ssid = state.wifiSsid || ''
    }
    if (!heaterEditing.value && Number.isFinite(state.heaterPower)) {
      heaterPower.value = Number(state.heaterPower)
    }
  },
  { immediate: true }
)

watch(
  () => tempEntries.value.length,
  (count) => {
    historySelection.tempIndices = normalizeHistorySelection(historySelection.tempIndices, count)
  }
)

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
  heaterEditing.value = false
}
function togglePidSensor(idx: number) {
  pidDirty.value = true
  if (pidForm.sensorIndices.includes(idx)) {
    pidForm.sensorIndices = pidForm.sensorIndices.filter((value) => value !== idx)
    return
  }
  pidForm.sensorIndices = normalizeIndices([...pidForm.sensorIndices, idx], tempEntries.value.length)
}
async function applyPid() {
  if (!deviceId.value) return
  const count = tempEntries.value.length
  const selected = normalizeIndices(pidForm.sensorIndices, count)
  pidForm.sensorIndices = selected
  if (count === 0) {
    pidApplyStatus.value = 'Нет доступных датчиков'
    return
  }
  if (selected.length === 0) {
    pidApplyStatus.value = 'Выберите хотя бы один датчик'
    return
  }
  pidApplyStatus.value = 'Сохраняю PID...'
  try {
    await store.pidApply(nuxtApp.$mqtt, deviceId.value, {
      setpoint: pidForm.setpoint,
      sensors: selected,
      kp: pidForm.kp,
      ki: pidForm.ki,
      kd: pidForm.kd,
    })
    pidDirty.value = false
    pidApplyStatus.value = 'PID сохранен'
  } catch (e: any) {
    pidApplyStatus.value = e?.message || 'Ошибка сохранения PID'
  }
}
async function enablePid() {
  if (!deviceId.value) return
  pidApplyStatus.value = 'Включаю PID...'
  try {
    await store.pidEnable(nuxtApp.$mqtt, deviceId.value)
    pidApplyStatus.value = 'PID включен'
  } catch (e: any) {
    pidApplyStatus.value = e?.message || 'Ошибка включения PID'
  }
}
async function disablePid() {
  if (!deviceId.value) return
  pidApplyStatus.value = 'Выключаю PID...'
  try {
    await store.pidDisable(nuxtApp.$mqtt, deviceId.value)
    pidApplyStatus.value = 'PID выключен'
  } catch (e: any) {
    pidApplyStatus.value = e?.message || 'Ошибка выключения PID'
  }
}
async function applyWifi() {
  if (!deviceId.value) return
  wifiApplyStatus.value = 'Применяю Wi‑Fi...'
  try {
    await store.wifiApply(nuxtApp.$mqtt, deviceId.value, {
      mode: wifiForm.mode,
      ssid: wifiForm.ssid,
      password: wifiForm.password,
    })
    wifiDirty.value = false
    wifiApplyStatus.value = 'Wi‑Fi обновлен (устройство может переподключиться)'
  } catch (e: any) {
    wifiApplyStatus.value = e?.message || 'Ошибка применения Wi‑Fi'
  }
}

function toggleHistoryTemp(idx: number) {
  if (historySelection.tempIndices.includes(idx)) {
    if (historySelection.tempIndices.length <= 1) {
      return
    }
    historySelection.tempIndices = historySelection.tempIndices.filter((value) => value !== idx)
  } else {
    historySelection.tempIndices = normalizeIndices([...historySelection.tempIndices, idx], tempEntries.value.length)
  }
}

async function loadHistory() {
  if (!deviceId.value) return
  historyLoading.value = true
  historyStatus.value = ''
  try {
    const params = new URLSearchParams({
      device_id: deviceId.value,
      limit: String(historyFilters.limit),
    })
    if (historyFilters.from) {
      params.set('from', new Date(historyFilters.from).toISOString())
    }
    if (historyFilters.to) {
      params.set('to', new Date(historyFilters.to).toISOString())
    }
    const data = await apiFetch<MeasurementRow[]>(`/api/measurements?${params.toString()}`)
    historyData.value = data
    historyStatus.value = `Получено ${data.length} точек`
  } catch (e: any) {
    historyStatus.value = e?.message || 'Не удалось загрузить историю'
  } finally {
    historyLoading.value = false
  }
}

onMounted(() => {
  historyFilters.to = toLocalInputValue(new Date())
  historyFilters.from = toLocalInputValue(new Date(Date.now() - 60 * 60 * 1000))
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
.chip.warn { background: #fff4e5; color: #a55a00; border-color: rgba(247, 178, 56, 0.35); }
.grid { display: grid; grid-template-columns: repeat(2, minmax(280px, 1fr)); gap: 16px; }
@media (max-width: 900px) {
  .grid { grid-template-columns: 1fr; }
}
.inline { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.inline > * { min-width: 0; }
.fields > * { flex: 1 1 180px; }
.fields .compact input { width: 100%; }
.range-row { display: flex; align-items: center; gap: 10px; }
.range-row input[type="range"] { flex: 1; }
.divider { height: 1px; background: var(--border); margin: 12px 0; }
.chip-select { display: flex; flex-wrap: wrap; gap: 8px; }
.chip-option { background: #f1f3f4; border: 1px solid var(--border); color: #1f2d3d; border-radius: 999px; padding: 6px 10px; font-size: 12px; font-weight: 600; cursor: pointer; }
.chip-option.selected { background: #e6f8ed; border-color: rgba(46, 139, 87, 0.35); color: #2e8b57; }
.checkbox { display: inline-flex; align-items: center; justify-content: flex-start; gap: 10px; font-weight: 600; white-space: nowrap; }
.checkbox input { margin: 0; width: 18px; height: 18px; accent-color: var(--accent); flex-shrink: 0; }
.compact input { width: 120px; }
.form-group { display: flex; flex-direction: column; gap: 6px; }
label { font-size: 14px; font-weight: 600; color: #1f1f1f; }
input { width: 100%; box-sizing: border-box; }
.actions { display: flex; gap: 10px; flex-wrap: wrap; }
.muted { color: var(--muted); font-size: 13px; }
.chart-stack { display: flex; flex-direction: column; gap: 16px; }
.chart-box { background: #f8fafc; border-radius: 12px; border: 1px solid var(--border); padding: 12px; height: 360px; display: flex; flex-direction: column; gap: 8px; }
.chart-box :deep(.js-plotly-plot) { width: 100%; height: 100%; flex: 1; }
.temps { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; margin-top: 6px; }
.temp-card { background: linear-gradient(180deg, #f9fbff, #eef3fb); border: 1px solid var(--border); border-radius: 12px; padding: 10px; box-shadow: 0 2px 6px rgba(52, 152, 219, 0.08); font-variant-numeric: tabular-nums; min-height: 78px; display: flex; flex-direction: column; gap: 4px; }
.temp-card.subtle { background: #f7f7f7; box-shadow: none; }
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
