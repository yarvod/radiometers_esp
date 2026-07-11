<template>
  <div class="device-feature-stack">
    <div class="card metrics">
      <div class="metrics-top"><h3>Показания</h3><div class="readings-grid large">
        <div v-for="entry in adcReadings" :key="entry.key" class="reading-card primary"><div class="reading-label">{{ entry.label }}</div><div class="reading-value big">{{ format(entry.value, 6) }}</div><div class="reading-sub">В</div></div>
      </div></div>
      <div class="temps">
        <div class="temp-card subtle"><div class="temp-title">Wi-Fi RSSI</div><div class="temp-value small">{{ state.wifiRssi ?? '--' }} dBm</div></div>
        <div class="temp-card subtle"><div class="temp-title">Wi-Fi качество</div><div class="temp-value small">{{ state.wifiQuality ?? '--' }}%</div></div>
        <div v-for="sensor in temps" :key="sensor.key" class="temp-card"><div class="temp-title">{{ sensor.label }}</div><div class="temp-value small">{{ format(sensor.value, 2) }} °C</div></div>
        <div class="temp-card"><div class="temp-title">INA U</div><div class="temp-value small">{{ format(state.inaBusVoltage, 3) }} V</div></div>
        <div class="temp-card"><div class="temp-title">INA I</div><div class="temp-value small">{{ format(state.inaCurrent, 3) }} A</div></div>
        <div class="temp-card"><div class="temp-title">INA P</div><div class="temp-value small">{{ format(state.inaPower, 3) }} W</div></div>
        <div class="temp-card warm"><div class="temp-title">Нагрев</div><div class="temp-value small">{{ format(state.heaterPower, 1) }} %</div></div>
        <div class="temp-card cool"><div class="temp-title">Вентилятор</div><div class="temp-value small">{{ format(state.fanPower, 1) }} %</div></div>
        <div class="temp-card subtle"><div class="temp-title">FAN1</div><div class="temp-value small">{{ state.fan1Rpm ?? '—' }} rpm</div></div>
        <div class="temp-card subtle"><div class="temp-title">FAN2</div><div class="temp-value small">{{ state.fan2Rpm ?? '—' }} rpm</div></div>
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="card-head"><h3>Логи</h3><span class="badge">Файлы</span></div>
        <div class="form-group"><label>Имя файла</label><input v-model="log.filename" /></div>
        <div class="inline fields"><label class="checkbox"><input type="checkbox" v-model="log.useMotor" /><span>Использовать мотор</span></label><label class="compact">Длительность, с<input type="number" min="0.1" step="0.1" v-model.number="log.durationSec" /></label></div>
        <div class="actions"><button class="btn primary" @click="startLog">Старт</button><button class="btn warning ghost" @click="stopLog">Стоп</button></div>
        <p class="muted" v-if="logStatus">{{ logStatus }}</p><p class="muted">Текущий файл: {{ state.logFilename || '—' }}</p>
        <div class="status-row"><span class="chip subtle">SD: {{ storageUsage }}</span><span class="chip subtle">root: {{ optionalCount(state.sdRootDataFiles) }}</span><span class="chip subtle">to_upload: {{ optionalCount(state.sdToUploadFiles) }}</span><span class="chip subtle">uploaded: {{ optionalCount(state.sdUploadedFiles) }}</span></div>
      </div>

      <div class="card">
        <div class="card-head"><h3>Система</h3><span class="badge">Управление</span></div>
        <div class="status-row"><span class="chip" :class="{ online: externalPowerOn === true, subtle: externalPowerOn === null }">EXT_PWR_ON: {{ externalPowerOn === null ? '—' : externalPowerOn ? 'ON' : 'OFF' }}</span><span class="chip subtle">Heap: {{ formatBytes(state.heapFreeBytes) }}</span><span class="chip subtle">Largest: {{ formatBytes(state.heapLargestFreeBlockBytes) }}</span><span v-if="state.minioEnabled === false" class="chip subtle">MinIO выключен</span><template v-else-if="hasMinioOutcomeCounters"><span class="chip subtle">MinIO попыток с запуска: {{ optionalCount(state.minioUploadAttempts) }}</span><span class="chip success">успешных PUT: {{ optionalCount(state.minioUploadSuccesses) }}</span><span class="chip" :class="Number(state.minioUploadFailures) > 0 ? 'danger' : 'subtle'">ошибок загрузки: {{ optionalCount(state.minioUploadFailures) }}</span><span v-if="Number(state.minioArchiveFailures) > 0" class="chip danger">ошибок архива: {{ optionalCount(state.minioArchiveFailures) }}</span><span class="chip subtle">последний PUT: {{ minioLastAttemptLabel }}</span></template><span v-else class="chip subtle">MinIO статистика: нужна новая прошивка</span></div>
        <div class="form-group"><label>Пауза EXT_PWR_ON, мс</label><input type="number" min="100" max="30000" step="100" v-model.number="externalPowerOffMs" /></div>
        <div class="actions"><button class="btn success" @click="setExternalPower(true)" :disabled="systemBusy">ON</button><button class="btn warning ghost" @click="setExternalPower(false)" :disabled="systemBusy">OFF</button><button class="btn primary" @click="cycleExternalPower" :disabled="systemBusy">Передернуть</button><button class="btn ghost" @click="syncConfig" :disabled="systemBusy">Синхр. config</button><button class="btn danger" @click="restart" :disabled="systemBusy">Перезагрузить</button></div>
        <p class="muted" v-if="systemStatus">{{ systemStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head"><h3>Термоконтроль</h3><span class="badge accent">Heater + PID</span></div>
        <div class="status-row"><span class="chip" :class="{ online: pidEnabled === true, subtle: pidEnabled === null }">PID {{ pidEnabled === null ? '—' : pidEnabled ? 'On' : 'Off' }}</span><span class="chip subtle">Выход: {{ format(state.pidOutput, 1) }}%</span><span class="chip subtle">Цель: {{ format(state.pidSetpoint, 1) }}°C</span></div>
        <div class="form-group"><label>Ручной нагрев, %</label><div class="range-row"><input type="range" min="0" max="100" step="0.5" v-model.number="heaterPower" @input="heaterDirty = true" /><input type="number" min="0" max="100" step="0.1" v-model.number="heaterPower" @input="heaterDirty = true" /></div></div>
        <button class="btn danger" @click="setHeater">Установить</button><div class="divider"></div>
        <div class="form-group"><label>PID цель, °C</label><input type="number" step="0.1" v-model.number="pid.setpoint" @input="pidDirty = true" /></div>
        <div class="form-group"><label>PID датчики</label><div class="chip-select"><button v-for="(sensor, index) in temps" :key="sensor.key" type="button" class="chip-option" :class="{ selected: pid.sensorIndices.includes(index) }" @click="togglePidSensor(index)">{{ sensor.label }}</button></div></div>
        <div class="inline fields"><label class="compact">Kp<input type="number" step="0.01" v-model.number="pid.kp" @input="pidDirty = true" /></label><label class="compact">Ki<input type="number" step="0.01" v-model.number="pid.ki" @input="pidDirty = true" /></label><label class="compact">Kd<input type="number" step="0.01" v-model.number="pid.kd" @input="pidDirty = true" /></label></div>
        <div class="diagnostic-grid"><div v-for="item in pidDiagnostics" :key="item.label" class="diagnostic-item"><span class="diagnostic-label">{{ item.label }}</span><span class="diagnostic-value">{{ item.value }}</span></div></div>
        <div class="actions"><button class="btn primary" @click="applyPid">Сохранить PID</button><button class="btn success" @click="setPidEnabled(true)">Включить</button><button class="btn warning ghost" @click="setPidEnabled(false)">Выключить</button></div><p class="muted" v-if="pidStatus">{{ pidStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head"><h3>Шаговик</h3><span class="badge success">Мотор</span></div>
        <div class="status-row"><span class="chip" :class="{ online: stepperEnabled === true, subtle: stepperEnabled === null }">{{ stepperEnabled === null ? '—' : stepperEnabled ? 'Enabled' : 'Disabled' }}</span><span class="chip subtle">{{ stepperMoving === null ? '—' : stepperMoving ? 'Moving' : 'Idle' }}</span><span class="chip subtle">Home: {{ state.stepperHomeStatus || '—' }}</span></div>
        <div class="actions"><button class="btn success" @click="stepperEnable">Enable</button><button class="btn ghost" @click="stepperDisable">Disable</button><button class="btn primary" @click="stepperHome">Найти 0 Hall+offset</button><button class="btn ghost" @click="stepperZero">Поставить 0</button></div>
        <div class="form-group"><label>Шаги</label><input type="number" v-model.number="stepper.steps" /></div><div class="form-group"><label>Скорость, us</label><input type="number" v-model.number="stepper.speedUs" @input="stepperDirty = true" /></div><div class="form-group"><label>Offset после Hall</label><input type="number" v-model.number="stepper.offsetSteps" @input="stepperDirty = true" /></div><div class="form-group"><label>Шагов до нагрузки</label><input type="number" min="1" max="20000" v-model.number="stepper.loggingMotorSteps" @input="stepperDirty = true" /></div>
        <div class="form-group"><label>Возврат</label><select v-model="stepper.loggingReturnMode" @change="stepperDirty = true"><option value="home">Hall + offset каждый цикл</option><option value="steps">Назад по шагам</option></select></div><label class="inline checkbox"><input type="checkbox" v-model="stepper.reverse" /><span>Реверс</span></label>
        <div class="actions"><button class="btn primary" @click="stepperMove">Движение</button><button class="btn ghost" @click="saveStepper">Сохранить настройки</button></div><p class="muted">Pos: {{ optionalValue(state.stepperPosition) }} Target: {{ optionalValue(state.stepperTarget) }}</p><p class="muted" v-if="stepperStatus">{{ stepperStatus }}</p>
      </div>

      <div class="card">
        <div class="card-head"><h3>Сеть</h3><span class="badge">Wi-Fi / Ethernet</span></div>
        <div class="status-row"><span class="chip strong">IP: {{ state.wifiIp || '--' }}</span><span class="chip subtle">STA: {{ state.wifiStaIp || '--' }}</span><span class="chip subtle">AP: {{ state.wifiApIp || '--' }}</span><span class="chip subtle">ETH: {{ state.ethIp || '--' }}</span></div>
        <div class="form-group"><label>Сетевой режим</label><select v-model="network.mode" @change="networkDirty = true"><option value="wifi">Только Wi-Fi</option><option value="eth">Только Ethernet</option><option value="both">Wi-Fi + Ethernet</option></select></div><div class="form-group"><label>Приоритет</label><select v-model="network.priority" @change="networkDirty = true"><option value="wifi">Wi-Fi</option><option value="eth">Ethernet</option></select></div><button class="btn primary" @click="applyNetwork">Применить сеть</button><p class="muted" v-if="networkStatus">{{ networkStatus }}</p><div class="divider"></div>
        <div class="form-group"><label>Wi-Fi режим</label><select v-model="wifi.mode" @change="wifiDirty = true"><option value="sta">STA</option><option value="ap">AP</option></select></div><div class="form-group"><label>SSID</label><input v-model="wifi.ssid" @input="wifiDirty = true" /></div><div class="form-group"><label>Пароль</label><input type="password" v-model="wifi.password" @input="wifiDirty = true" /></div><button class="btn primary" @click="applyWifi">Применить Wi-Fi</button><p class="muted" v-if="wifiStatus">{{ wifiStatus }}</p>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useDevicesStore } from '~/stores/devices'

type TempEntry = { key: string; label: string; address?: string; value: number | null }
const props = defineProps<{ deviceId: string; state: Record<string, any>; adcLabels: Record<string, string>; temps: TempEntry[] }>()
const store = useDevicesStore(); const nuxtApp = useNuxtApp(); store.init(nuxtApp.$mqtt)
const log = reactive({ filename: 'data', useMotor: false, durationSec: 1 })
const stepper = reactive({ steps: 400, speedUs: 1500, offsetSteps: 0, loggingMotorSteps: 100, loggingReturnMode: 'home', reverse: false })
const pid = reactive({ setpoint: 25, sensorIndices: [] as number[], kp: 1, ki: 0, kd: 0 })
const wifi = reactive({ mode: 'sta', ssid: '', password: '' }); const network = reactive({ mode: 'wifi', priority: 'wifi' })
const heaterPower = ref(0); const externalPowerOffMs = ref(1000); const systemBusy = ref(false)
const heaterDirty = ref(false); const pidDirty = ref(false); const stepperDirty = ref(false); const wifiDirty = ref(false); const networkDirty = ref(false)
const logStatus = ref(''); const systemStatus = ref(''); const pidStatus = ref(''); const stepperStatus = ref(''); const wifiStatus = ref(''); const networkStatus = ref('')
const adcReadings = computed(() => ['adc1', 'adc2', 'adc3'].map((key, index) => ({ key, label: props.adcLabels[key] || key.toUpperCase(), value: props.state[`voltage${index + 1}`] })))
const externalPowerOn = computed<boolean | null>(() => typeof props.state.externalPowerOn === 'boolean' ? props.state.externalPowerOn : null)
const pidEnabled = computed<boolean | null>(() => typeof props.state.pidEnabled === 'boolean' ? props.state.pidEnabled : null)
const stepperEnabled = computed<boolean | null>(() => typeof props.state.stepperEnabled === 'boolean' ? props.state.stepperEnabled : null)
const stepperMoving = computed<boolean | null>(() => typeof props.state.stepperMoving === 'boolean' ? props.state.stepperMoving : null)
const hasMinioOutcomeCounters = computed(() => Number.isFinite(Number(props.state.minioUploadSuccesses)) && Number.isFinite(Number(props.state.minioUploadFailures)))
const storageUsage = computed(() => { const total = Number(props.state.sdTotalBytes || 0); const used = Number(props.state.sdUsedBytes || 0); return total > 0 ? `${formatBytes(used)} / ${formatBytes(total)}` : '—' })
const format = (value: unknown, digits: number) => {
  if (value === null || value === undefined || value === '') return '—'
  return Number.isFinite(Number(value)) ? Number(value).toFixed(digits) : '—'
}
const formatBytes = (value: unknown) => { const bytes = Number(value); if (!Number.isFinite(bytes) || bytes <= 0) return '—'; if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(1)} GB`; if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MB`; return `${(bytes / 1024).toFixed(1)} KB` }
const optionalCount = (value: unknown) => value === null || value === undefined || value === '' || !Number.isFinite(Number(value)) ? '—' : String(Math.max(0, Math.trunc(Number(value))))
const optionalValue = (value: unknown) => value === null || value === undefined || value === '' ? '—' : String(value)
const minioLastAttemptLabel = computed(() => {
  const attempts = Number(props.state.minioUploadAttempts)
  const attemptMs = Number(props.state.minioLastAttemptMs)
  if (!Number.isFinite(attempts) || attempts <= 0 || !Number.isFinite(attemptMs) || attemptMs <= 0) return '—'
  const successMs = Number(props.state.minioLastSuccessMs)
  const failureMs = Number(props.state.minioLastFailureMs)
  const succeeded = Number.isFinite(successMs) && successMs >= attemptMs && successMs >= failureMs
  const failed = Number.isFinite(failureMs) && failureMs >= attemptMs
  const resultMs = succeeded ? successMs : failed ? failureMs : attemptMs
  const uptimeMs = Number(props.state.uptimeMs)
  const age = Number.isFinite(uptimeMs) && uptimeMs >= resultMs
    ? `${Math.round((uptimeMs - resultMs) / 1000)} сек назад`
    : `на ${Math.round(resultMs / 1000)}-й секунде после запуска`
  return succeeded ? `успешно, ${age}` : failed ? `ошибка, ${age}` : age
})
const message = (error: any, fallback: string) => error?.message || fallback
const pidDiagnostics = computed(() => [
  { label: 'PID temp', value: `${format(props.state.pidTemperature, 2)} °C` },
  { label: 'Ошибка', value: `${format(props.state.pidError, 3)} °C` },
  { label: 'P', value: `${format(props.state.pidPTerm, 2)} %` },
  { label: 'I', value: `${format(props.state.pidITerm, 2)} %` },
  { label: 'D', value: `${format(props.state.pidDTerm, 2)} %` },
  { label: 'Raw', value: `${format(props.state.pidRawOutput, 2)} %` },
  { label: 'Integral', value: `${format(props.state.pidIntegral, 2)} C*s` },
  { label: 'dError/dt', value: `${format(props.state.pidDerivative, 4)} C/s` },
  { label: 'dt', value: `${format(props.state.pidDt, 3)} s` },
  { label: 'Saturation', value: pidEnabled.value === null ? '—' : pidEnabled.value ? (props.state.pidSaturatedHigh ? 'HIGH' : props.state.pidSaturatedLow ? 'LOW' : 'NO') : 'OFF' },
  { label: 'Integrator', value: pidEnabled.value === null ? '—' : pidEnabled.value ? (props.state.pidIntegralHeld ? 'HELD' : 'UPDATING') : 'OFF' },
])

const startLog = async () => { try { await store.logStart(nuxtApp.$mqtt, props.deviceId, log); logStatus.value = 'Логирование запущено' } catch (e) { logStatus.value = message(e, 'Ошибка запуска') } }
const stopLog = async () => { try { await store.logStop(nuxtApp.$mqtt, props.deviceId); logStatus.value = 'Логирование остановлено' } catch (e) { logStatus.value = message(e, 'Ошибка остановки') } }
const systemAction = async (label: string, action: () => Promise<any>) => { systemBusy.value = true; systemStatus.value = label; try { await action(); systemStatus.value = 'Команда выполнена' } catch (e) { systemStatus.value = message(e, 'Ошибка команды') } finally { systemBusy.value = false } }
const restart = () => {
  if (!window.confirm('Перезагрузить устройство? Текущая запись может прерваться.')) return
  return systemAction('Перезагружаю...', () => store.restartDevice(nuxtApp.$mqtt, props.deviceId))
}
const setExternalPower = (enabled: boolean) => {
  if (!enabled && !window.confirm('Выключить питание внешних модулей?')) return
  return systemAction('Переключаю питание...', () => store.externalPowerSet(nuxtApp.$mqtt, props.deviceId, enabled))
}
const cycleExternalPower = () => {
  const offMs = Math.max(100, Number(externalPowerOffMs.value) || 1000)
  if (!window.confirm(`Передернуть питание внешних модулей? OFF на ${offMs} мс.`)) return
  return systemAction('Передергиваю питание...', () => store.externalPowerCycle(nuxtApp.$mqtt, props.deviceId, offMs))
}
const syncConfig = () => systemAction('Синхронизирую config...', () => store.configSyncInternalFlash(nuxtApp.$mqtt, props.deviceId))
const setHeater = async () => { try { await store.heaterSet(nuxtApp.$mqtt, props.deviceId, heaterPower.value); heaterDirty.value = false; pidStatus.value = 'Нагрев установлен' } catch (e) { pidStatus.value = message(e, 'Ошибка нагрева') } }
const togglePidSensor = (index: number) => { pid.sensorIndices = pid.sensorIndices.includes(index) ? pid.sensorIndices.filter((item) => item !== index) : [...pid.sensorIndices, index]; pidDirty.value = true }
const applyPid = async () => { if (!pid.sensorIndices.length) { pidStatus.value = 'Выберите хотя бы один датчик'; return } try { await store.pidApply(nuxtApp.$mqtt, props.deviceId, { setpoint: pid.setpoint, sensors: pid.sensorIndices, kp: pid.kp, ki: pid.ki, kd: pid.kd }); pidDirty.value = false; pidStatus.value = 'PID сохранен' } catch (e) { pidStatus.value = message(e, 'Ошибка PID') } }
const setPidEnabled = async (enabled: boolean) => { try { enabled ? await store.pidEnable(nuxtApp.$mqtt, props.deviceId) : await store.pidDisable(nuxtApp.$mqtt, props.deviceId); pidStatus.value = enabled ? 'PID включен' : 'PID выключен' } catch (e) { pidStatus.value = message(e, 'Ошибка PID') } }
const stepperCall = async (label: string, action: () => Promise<any>) => { stepperStatus.value = label; try { await action(); stepperStatus.value = 'Команда отправлена' } catch (e) { stepperStatus.value = message(e, 'Ошибка мотора') } }
const stepperEnable = () => stepperCall('Включаю...', () => store.stepperEnable(nuxtApp.$mqtt, props.deviceId)); const stepperDisable = () => stepperCall('Выключаю...', () => store.stepperDisable(nuxtApp.$mqtt, props.deviceId)); const stepperHome = () => stepperCall('Ищу Hall...', () => store.stepperFindZero(nuxtApp.$mqtt, props.deviceId)); const stepperZero = () => stepperCall('Ставлю 0...', () => store.stepperZero(nuxtApp.$mqtt, props.deviceId))
const stepperMove = () => stepperCall('Двигаю...', () => store.stepperMove(nuxtApp.$mqtt, props.deviceId, { steps: stepper.steps, reverse: stepper.reverse, speedUs: stepper.speedUs }))
const saveStepper = () => stepperCall('Сохраняю...', async () => { await store.stepperSettings(nuxtApp.$mqtt, props.deviceId, { speedUs: Math.max(1, stepper.speedUs), offsetSteps: stepper.offsetSteps, loggingMotorSteps: Math.min(20000, Math.max(1, stepper.loggingMotorSteps)), loggingHomeEachCycle: stepper.loggingReturnMode !== 'steps' }); stepperDirty.value = false })
const applyWifi = async () => { try { await store.wifiApply(nuxtApp.$mqtt, props.deviceId, wifi); wifiDirty.value = false; wifiStatus.value = 'Wi-Fi обновлен' } catch (e) { wifiStatus.value = message(e, 'Ошибка Wi-Fi') } }
const applyNetwork = async () => { try { await store.netApply(nuxtApp.$mqtt, props.deviceId, network); networkDirty.value = false; networkStatus.value = 'Сетевой режим обновлен' } catch (e) { networkStatus.value = message(e, 'Ошибка сети') } }

const seed = () => {
  const state = props.state
  if (!heaterDirty.value) heaterPower.value = Number(state.heaterPower || 0)
  if (!pidDirty.value) {
    pid.setpoint = Number(state.pidSetpoint ?? pid.setpoint); pid.kp = Number(state.pidKp ?? pid.kp); pid.ki = Number(state.pidKi ?? pid.ki); pid.kd = Number(state.pidKd ?? pid.kd)
    const mask = Number(state.pidSensorMask)
    const indices = Number.isInteger(mask) && mask > 0
      ? Array.from({ length: props.temps.length }, (_, index) => index).filter((index) => (mask & (1 << index)) !== 0)
      : Number.isInteger(Number(state.pidSensorIndex)) && Number(state.pidSensorIndex) >= 0
        ? [Number(state.pidSensorIndex)]
        : []
    pid.sensorIndices = indices.filter((index: number) => index >= 0 && index < props.temps.length)
  }
  if (!stepperDirty.value) { stepper.speedUs = Number(state.stepperSpeedUs ?? stepper.speedUs); stepper.offsetSteps = Number(state.stepperHomeOffsetSteps ?? stepper.offsetSteps); stepper.loggingMotorSteps = Number(state.loggingMotorSteps ?? stepper.loggingMotorSteps); stepper.loggingReturnMode = state.loggingHomeEachCycle === false ? 'steps' : 'home' }
  if (!wifiDirty.value) { wifi.mode = (state.wifiMode === 'ap' || state.wifiApMode === true) ? 'ap' : 'sta'; wifi.ssid = String(state.wifiSsid || '') }
  if (!networkDirty.value) { network.mode = String(state.netMode || 'wifi'); network.priority = state.netPriority === 'eth' ? 'eth' : 'wifi' }
}
watch(() => props.deviceId, () => { Object.values({ logStatus, systemStatus, pidStatus, stepperStatus, wifiStatus, networkStatus }).forEach((item) => { item.value = '' }); heaterDirty.value = false; pidDirty.value = false; stepperDirty.value = false; wifiDirty.value = false; networkDirty.value = false; seed() })
watch(() => props.state, seed, { deep: true, immediate: true })
</script>
